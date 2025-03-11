#include "AutomateAnnotations.h"

#include <map>
#include <string>

#include "Error.h"
#include "Expr.h"
#include "Function.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "IRVisitor.h"
#include "Simplify.h"
#include "Substitute.h"
#include "CodeGen_C.h"

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::tuple;
using std::vector;

Expr null(){
    return Call::make(Handle(), Call::null, {}, Call::Intrinsic);
}

Expr pointer_length(Expr e){
    return Call::make(Int(32), Call::pointer_length, {e}, Call::Intrinsic);
}

Expr Perm(Expr array, Expr write){
    return Call::make(Resource(), Call::perm, {array, write}, Call::Intrinsic);
}

Expr trigger(Expr e){
    return Call::make(e.type(), Call::trigger, {e}, Call::Intrinsic);
}

Annotation context_everywhere(Expr e){
    return AnnExpr::make(AnnotationType::ContextEverywhere, e);
}

class FindFunctionCalls : public IRVisitor {
    const string &func;
    map<string, Function> &env;

    using IRVisitor::visit;

    void visit(const Call *op) override {
        // We don't care if the function was not user defined
        if (op->call_type != Call::Halide) {
            IRVisitor::visit(op);
            return;
        }
        // If we call our selves, sure
        if(op->name == func){
            IRVisitor::visit(op);
            return;
        }
        
        map<string, Function>::iterator called_func_it = env.find(op->name);
        if (called_func_it == env.end()) {
            internal_error << "Function " << op->name << "was called, but could not find a definition for it";
        }

        
        Function called_func = called_func_it->second;
        // If the function was inlined, we record that
        bool inlined = false;
        if (called_func.schedule().compute_level().is_inlined() && called_func.can_be_inlined()) {
            inlined = true;
        }

        called_funcs.emplace_back(called_func, inlined, op->args);
        IRVisitor::visit(op);
    }

public:
    // The function, if it was inlined and the arguments by with it was called
    vector<tuple<Function, bool, vector<Expr>>> called_funcs;

    FindFunctionCalls(const string &f, map<string, Function> &e)
        : func(f), env(e) {
    }
};

// class AnnExprTo : public IRMutator {
//     using IRMutator::visit;

//     AnnotationType t;

//     Annotation visit(const AnnExpr *op) override {
//         return AnnExpr::make(t, op->condition);
//     }

// public:
//     AnnExprTo(AnnotationType t = AnnotationType::Require) : t(t) {}
// };

// // Replace calls in annotations with 'func' to 'new_func'
// class ReplaceEnsureFunctionCall : public IRMutator {
//     const string &func;
//     const string &new_func;

//     using IRMutator::visit;

//     Expr visit(const Call *op) override {
//         //We found a call to the function!
//         if (op->name == func) {
//             return Call::make(op->type, new_func, op->args, op->call_type,
//                             op->func, op->value_index, op->image, op->param);
//         }

//         return IRMutator::visit(op);
//     }

//     Annotation visit(const AnnExpr *op) override {
//         // We are only interested if we have an ensure annotation
//         if (op->ann_type != AnnotationType::Ensure) {
//             return op;
//         }
//         return IRMutator::visit(op);
//     }

//     Annotation visit(const Permission *op) override {
//         // We are only interested in annotatated expressions
//         return op;
//     }

// public:

//     ReplaceEnsureFunctionCall(const string &f, const string &new_f)
//         : func(f), new_func(new_f) {
//     }
// };

class FindReductionVars : public IRVisitor {
public:
    FindReductionVars() : rvar_found(false) { }
    bool rvar_found;

    using IRVisitor::visit;

    void visit(const Variable *op) override {
        if(op->reduction_domain.defined())
            rvar_found = true;
    }
};

bool has_reduction_var(Expr e){
    FindReductionVars fp;
    e.accept(&fp);
    return fp.rvar_found;
    return false;
}

class AutomaticAnnotations {
    // Function done processing
    set<string> processed_functions;
    // Functions that are being processed, we can check this to make sure there are no cycles in the call graph
    set<string> busy_processing;
    map<string, Function> &env;
    vector<Function> &output_funcs;

    void fix_annotations(Function func, Definition def, vector<Expr> &def_args){
        std::vector<pair<string, Expr>> lets;
        for(const auto &v: def.values()){
            Expr body = v;
            while(const auto *op = body.as<Let>()){
                lets.emplace_back(std::make_pair(op->name, op->value));
                body = op->body;
            }
        }
        for(int i = (int)lets.size()-1; i>=0; i--){
            for(Expr &a: def_args){
                a = substitute(lets[i].first, lets[i].second, a);
            }
        }
    }

    bool is_output(Function func){
        for(const auto &f :output_funcs){
            if(f.same_as(func))
                return true;
        }
        return false;
    }

    void get_forall_bounds(Function func, vector<Expr> def_args, vector<string> &forall_vars, vector<Expr> &call_args,
            Expr &bounds, Expr &not_def_bound, map<string, Expr> &replacement){
        vector<Expr> pure_args = func.definition().args();        
        for(size_t i = 0; i < def_args.size(); i++){
            // We are going to forall over all inpure arguments
            Expr min, upper, extent;
            string dim = func.args()[i];
            // For output functions, we can just take the loop min/max 
            if(is_output(func)){
                min = Variable::make(Int(32), dim + ".loop_min");
                extent = Variable::make(Int(32), dim + ".loop_extent");
            } else {
                // Here we fill in this realized after bounds inferencing, since of compute_with scheduling directive
                // Makes the loops not say everything
                min = Variable::make(Int(32), dim + ".min_realized");
                extent = Variable::make(Int(32), dim + ".extent_realized");
            }
            upper = min + extent;
            if(!equal(pure_args[i], def_args[i])) {
                string forall_var = dim + ".forall";
                Expr forall_v = Variable::make(Int(32), forall_var);
                forall_vars.emplace_back(forall_var);
                string prefix = func.name() + ".s0." + dim;
                
                Expr new_bound = min <= forall_v && forall_v < upper;
                Expr new_not_def_bound = forall_v != def_args[i];
                replacement[func.args()[i]] = forall_v;
                if(!bounds.defined()){
                    bounds = new_bound;
                } else {
                    bounds = bounds && new_bound;
                }
                if(!not_def_bound.defined()){
                    not_def_bound = new_not_def_bound;
                } else {
                    not_def_bound = not_def_bound && new_not_def_bound;
                }
                call_args.emplace_back(forall_v);
            } else {
                call_args.emplace_back(pure_args[i]);
            }
            // call_args.emplace_back(min);
            // call_args.emplace_back(extent);
        }
        if(!bounds.defined())
            bounds = make_bool(true);
        if(!not_def_bound.defined())
            not_def_bound = make_bool(true);
    }

    void add_definition_annotations(Function func, Definition def, bool inlined){
        if(inlined){
            // Clear all the annotations added, since we use the definition of inline functions, so we don't have to prove things
            def.annotations().clear();
            return;
        }

        vector<Expr> pure_args = func.definition().args();
        vector<Expr> def_args = def.args();
        fix_annotations(func, def, def_args);

        vector<string> forall_vars;
        Expr bounds, not_def_bounds;
        vector<Expr> call_args;
        map<string, Expr> replacement;
        get_forall_bounds(func, def_args, forall_vars, call_args, bounds, not_def_bounds, replacement);

        // Check if we have any reduction vars. 
        // The last rvar should be at its max and the other at the min, for the post-condition to hold
        bool has_rvar = !def.schedule().rvars().empty();
        map<string, Expr> rvar_replacement;
        for(size_t i = 0; i<def.schedule().rvars().size(); i++){
            ReductionVariable rv = def.schedule().rvars()[i];
            if(i != def.schedule().rvars().size()-1){
                rvar_replacement[rv.var] = rv.min;
            } else {
                rvar_replacement[rv.var] = rv.min + rv.extent;
            }
        }

        vector<Annotation> new_def_annotations;

        if(has_rvar){
            // Reduction, just give all write permissions, since loops are serial anyway
            for(int i=0; i<func.outputs(); i++){
                Expr call = Call::make(func, call_args, i);
                Expr perm = Perm(call, write());
                Expr f = forall(forall_vars, bounds, perm);
                new_def_annotations.emplace_back(AnnExpr::make(AnnotationType::Context, f));
            }
        } else {
            // Non-reduction case: Add our own write permission
            for(int i=0; i<func.outputs(); i++){
                Expr call = Call::make(func, def_args, i);
                Expr perm = Perm(call, write());
                new_def_annotations.emplace_back(AnnExpr::make(AnnotationType::Context, perm));
            }
            // Add read permission for everything else (update definitions)
            if(!forall_vars.empty()){
                for(int i=0; i<func.outputs(); i++){
                    Expr call = Call::make(func, call_args, i);                    
                    Expr perm = Perm(call, Frac::make(1, 2));
                    Expr f = forall(forall_vars, bounds && not_def_bounds, perm);

                    new_def_annotations.emplace_back(AnnExpr::make(AnnotationType::Context, f));
                }
            }
        }

        for(const Annotation &ann: def.annotations()){
            const AnnExpr* ae = ann.as<AnnExpr>();
            internal_assert(ae);

            Expr new_condition = ae->condition;
            if(!forall_vars.empty()){
                new_condition = substitute(replacement, new_condition);
                new_condition = forall(forall_vars, bounds, new_condition);
            }

            AnnotationType new_ann_type;
            if(has_rvar){
                if(ae->ann_type == AnnotationType::LoopInvariant){
                    new_ann_type = AnnotationType::LoopInvariant;
                } else {
                    user_assert(ae->ann_type == AnnotationType::Ensure) 
                        << "Only ensure or invariant annotations are allowed for reduction functions";
                    new_ann_type = AnnotationType::Ensure;
                }
            } else {
                user_assert(ae->ann_type == AnnotationType::Ensure) << "Only ensure annotations are allowed for normal functions";
                new_ann_type = AnnotationType::Ensure;
            }
            new_def_annotations.emplace_back(AnnExpr::make(new_ann_type, new_condition));
        }
        

        // Check the annotations so far of the function
        // We already know that all the annotations are ensure expressions
        for (auto const &ann : func.func_annotations()) {
            const AnnExpr* ae = ann.as<AnnExpr>();
            internal_assert(ae && ae->ann_type == AnnotationType::Ensure);
            Expr new_cond = ae->condition;

            if(!forall_vars.empty()){
                new_cond = substitute(replacement, ae->condition);
                new_cond = forall(forall_vars, bounds, new_cond);
            }

            if(!is_const_true(new_cond)){
                new_def_annotations.emplace_back(AnnExpr::make(AnnotationType::Require, new_cond));
            }
        }

        // Clear out the old annotations, they are not valid anymore
        func.clear_func_annotations();

        // Now add our own ensure expression definitions to the function annotation
        for (auto &ann : def.annotations()){
            const AnnExpr* ae = ann.as<AnnExpr>();
            if(ae && ae->ann_type == AnnotationType::Ensure && ae->condition.type().is_bool()){
                user_assert(!has_reduction_var(ae->condition)) << "Ensure annotation of reduction cannot mention reduction variable";
                func.add_func_annotation(ann);
            } else if(ae && ae->ann_type == AnnotationType::LoopInvariant && ae->condition.type().is_bool()){
                func.add_func_annotation(AnnExpr::make(AnnotationType::Ensure, substitute(rvar_replacement, ae->condition)));
            }
        }

        def.annotations().clear();
        def.annotations() = new_def_annotations;
    }

    void add_function_annotations(Function func){
        string name = func.name();
        if (!func.definition().defined())
            user_error << "Function '" << name << "' doesn't have a definition.\n";
        // If this function was already processed, no need to look at it again.
        if(processed_functions.count(name) != 0) return;

        if(busy_processing.count(name) != 0)
            user_error << "Function '" << name << "' has a cycle dependency (it calls a function that depends on it).\n";

        debug(3) << "Automatic Annotations: checking function " << func.name() << "\n";
        busy_processing.emplace(name);
        Definition def = func.definition();

        bool inlined = false;
        if (func.schedule().compute_level().is_inlined() && func.can_be_inlined()) {
            inlined = true;
        }

        const Call *wrapper = func.is_wrapper();
        // TODO (Lars): Look into when this is needed, I think it had to do something with "compute_at"
        // with shared memory where I tried to do this correctly
        if(false && wrapper != nullptr){
            if(wrapper->call_type == Call::CallType::Image){
                // AnnExprTo annTo(AnnotationType::Ensure);
                // for(auto &ann: wrapper->param.annotations()){
                //     func.add_func_annotation(annTo.mutate(ann));
                // }
                busy_processing.erase(name);
                processed_functions.emplace(name);
                // func.sort_annotations();
                debug(2) << "Processed non-function " << func.name() << "\n"
                    << func << "\n";
                return;
            } else if(wrapper->call_type != Call::CallType::Halide){
                busy_processing.erase(name);
                processed_functions.emplace(name);
                // func.sort_annotations();
                debug(2) << "Processed non-function " << func.name() << "\n"
                    << func << "\n";
                return;
            }


            //The function was a wrapper, first visit the called function
            map<string, Function>::iterator called_func_it = env.find(wrapper->name);
            if (called_func_it == env.end())
                internal_error << "Function " << wrapper->name << " was called, but could not find a definition for it.";
            Function called_func = called_func_it->second;

            //TODO: maybe this is allowed.
            if (inlined)
                internal_error << "Function " << func.name() << "was called as wrapper function, but is inlined, which we do not support";
            
            // if (called_func.schedule().compute_level().is_inlined() && called_func.can_be_inlined())
            //     internal_error << "Function " << called_func.name() << " was called as wrapped function, but is inlined, which we do not support";

            add_function_annotations(called_func);
            // Now add the correct definitions
            vector<tuple<Function, bool, vector<Expr>>> call;
            call.emplace_back(called_func, false, wrapper->args);
            add_definition_annotations(func, def, false);

            // We add anotations that proved something about the called function, and prove the same thing
            // but now for the new wrapper_name function
            // for(Annotation a : called_func.func_annotations()){
            //     // ReplaceEnsureFunctionCall replace_call = ReplaceEnsureFunctionCall(called_func.name(), name);
            //     Annotation new_a = replace_call.mutate(a);
            //     func.add_func_annotation(new_a);
            //     func.add_annotation(new_a);
            // }
            // We are done with this function
            busy_processing.erase(name);
            processed_functions.emplace(name);
            func.sort_annotations();
            debug(2) << "Processed function " << func.name() << "\n"
              << func << "\n";
            return;
        }

        FindFunctionCalls calls = FindFunctionCalls(name, env);
        def.accept(&calls);

        for(auto &called_f: calls.called_funcs)
            add_function_annotations(std::get<0>(called_f));

        add_definition_annotations(func, def, inlined);

        // Add update definitions annotations
        for(auto &update :func.updates()){
            FindFunctionCalls calls = FindFunctionCalls(name, env);
            update.accept(&calls);
            for(auto &called_f: calls.called_funcs) add_function_annotations(std::get<0>(called_f));
                // add_self_reference_annotations(func, update, calls.self_references);
                add_definition_annotations(func, update, inlined);
        }

        busy_processing.erase(name);
        processed_functions.emplace(name);
        func.sort_annotations();
        debug(2) << "Processed function " << func.name() << "\n"
                 << func << "\n";
    }
    
public:
    void add_automatic_annotations(){
        for (auto &iter : env) {
            Function &func = iter.second;
            add_function_annotations(func);
        }
    }

    AutomaticAnnotations(map<string, Function> &e, vector<Function> &output_funcs) : env(e), output_funcs(output_funcs) {};
    
};

void add_automatic_annotations(map<string, Function> &env, vector<Function> &output_funcs) {
    AutomaticAnnotations aa = AutomaticAnnotations(env, output_funcs);
    aa.add_automatic_annotations();
}

}  // namespace Internal
}  // namespace Halide

