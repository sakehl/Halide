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

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::tuple;
using std::vector;

class FindFunctionCalls : public IRVisitor {
    const string &func;
    map<string, Function> &env;

    using IRVisitor::visit;

    void visit(const Call *op) override {
        // We don't care if we the function was not user defined
        if (op->call_type != Call::Halide) {
            IRVisitor::visit(op);
            return;
        }
        // If we call our selves, record the arguments
        if(op->name == func){
            self_references.emplace_back(op->args);
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
    // Stores the arguments which the self reference to this function was made
    vector<vector<Expr>> self_references;

    FindFunctionCalls(const string &f, map<string, Function> &e)
        : func(f), env(e) {
    }
};

class IsEnsureExpressionWithCall : public IRVisitor {
    const string &func;

    using IRVisitor::visit;

    void visit(const Call *op) override {
        //We found a call to the function!
        if (op->name == func) {
            has_function_call = true;
            function_call_args.emplace_back(op->args);
            return;
        }

        IRVisitor::visit(op);
    }

    void visit(const AnnExpr *op) override {
        // We are only interested if we have an ensure annotation
        if (op->ann_type != AnnotationType::Ensure) {
            return;
        }

        IRVisitor::visit(op);
    }

    void visit(const Permission *op) override {
        // We are only interested in annotatated expressions
        return;
    }

public:
    bool has_function_call;
    vector<vector<Expr>> function_call_args;

    IsEnsureExpressionWithCall(const string &f)
        : func(f), has_function_call(false) {
    }
};

class AnnExprToRequire : public IRMutator {
    using IRMutator::visit;

    Annotation visit(const AnnExpr *op) override {
        return AnnExpr::make(AnnotationType::Require, op->condition);
    }
};

class ReplaceEnsureFunctionCall : public IRMutator {
    const string &func;
    const string &new_func;

    using IRMutator::visit;

    Expr visit(const Call *op) override {
        //We found a call to the function!
        if (op->name == func) {
            return Call::make(op->type, new_func, op->args, op->call_type,
                            op->func, op->value_index, op->image, op->param);
        }

        return IRMutator::visit(op);
    }

    Annotation visit(const AnnExpr *op) override {
        // We are only interested if we have an ensure annotation
        if (op->ann_type != AnnotationType::Ensure) {
            return op;
        }
        return IRMutator::visit(op);
    }

    Annotation visit(const Permission *op) override {
        // We are only interested in annotatated expressions
        return op;
    }

public:

    ReplaceEnsureFunctionCall(const string &f, const string &new_f)
        : func(f), new_func(new_f) {
    }
};

class AutomaticAnnotations {
    // Function done processing
    set<string> processed_functions;
    // Functions that are being processed, we can check this to make sure there are no cycles in the call graph
    set<string> busy_processing;
    map<string, Function> env;

    void add_self_reference_annotations(Function func, Definition def, vector<vector<Expr>> self_reference_args){
        /*
        For f(x,y) = 2*(x+y)+1;
            f.ensures(f(x,y) % 2 == 1);
            f(x, y) = f(x,y)*3 + 1;
            f.ensures((f(x,y) % 2 == 0));
            f(x,x) = 0;
            f.ensures((f(x,x) == 0));
        We'd like to end up with
          f_def:
           context Perm(f(x,y), write);
           ensures f(x,y) % 2 == 1;
          f_update_0:
           context Perm(f(x,y), write);
           ensures f(x,y) % 2 == 0;
          f_update_1:
           context Perm(f(x,x), write);
           ensures f(x,x) == 0;
          f_total
           ensures y==x ==> f(x,y) == 0;
           ensures y!=x ==> f(x,y) % 2 == 0;

          We do that by getting information from the f_total (annotations that are attachted to the Function not the Definitions)
          for each definition, adding the correct requires concerning the previous definition and afterwards updating the f_total
          for what it proves now.
        */

        vector<Expr> pure_args = func.definition().args();
        vector<Expr> def_args = def.args();
        vector<vector<Expr>> prev_self_references;
        
        for(auto & self_ref: self_reference_args){
            // First add the permission to read our self_references.
            // We only need read permissions, thus we give that.
            // But we need to make sure to not add them doubly (from our own write permission)
            // Read permissions for a function without args 'g()' are not needed (we already have them)
            // Example:
            // f(x,y) = x+y;
            // f(x, 0) = f(x,x) + 1;
            // We get (x!=x or y!=0) ==> Perm(f(x,0), read);

            Expr condition = const_false();
            for(unsigned long i = 0; i<self_ref.size(); i++) {
                condition = Or::make(condition, NE::make(self_ref[i], def_args[i]) );
            }

            // Now check the previous self reference args, we also don't want to be equal to them
            // Otherwise f(x,0) = f(x,1) + f(x,1) leads to a double write permission
            for(auto & prev_args: prev_self_references){
                Expr new_cond = const_false();
                for(unsigned long i = 0; i<self_ref.size(); i++){
                    new_cond = Or::make(new_cond, NE::make(self_ref[i], prev_args[i]) );
                }
                condition = And::make(condition, new_cond);
            }

            prev_self_references.emplace_back(self_ref);

            condition = simplify(condition);
            if(!is_const_false(condition)){
                Expr call = Call::make(func, self_ref);
                def.add_annotation(Permission::make(AnnotationType::Context, condition, call, ReadPerm::make(), {}));
            }

            // Check if we have any reduction vars. This means we only want requirements for the first step
            // of the reduction for_loop, otherwise our self_references could be already updated
            Expr rvar_condition;
            bool has_rvar = false;
            for (const ReductionVariable &rv : def.schedule().rvars()) {
                Expr new_cond = EQ::make(Variable::make(Int(32), rv.var), rv.min);
                if(has_rvar){
                    rvar_condition = And::make(rvar_condition, new_cond);
                } else {
                    rvar_condition = new_cond;
                    has_rvar = true;
                }
            }
            
            // Check the annotations so far of the function
            // We already know that all the annotations are ensure expressions
            for (auto const &ann : func.func_annotations()) {
                // We need to replace the definition arguments, with the called arguments.
                std::map<string, Expr> replacer;
                int i = 0;
                for(auto const &a : pure_args){
                    const Variable* var = a.as<Variable>();
                    internal_assert(var);
                    replacer[var->name] = self_ref[i];
                    i++;
                }
                Annotation new_ann = AnnExprToRequire().mutate(substitute(replacer, ann));
                if(has_rvar) new_ann = add_antecedent(rvar_condition, new_ann);
                new_ann = simplify(new_ann);

                const AnnExpr* ae = new_ann.as<AnnExpr>();
                // This should hold, cause only inlined functions (with no updates ==> no self references) can have permissions
                internal_assert(ae);
                if(!is_const_true(ae->condition)){
                    // Let us now check the 
                    def.add_annotation(new_ann);
                }
            }
        }
    }

    void add_definition_annotations(Function func, Definition def, vector<tuple<Function, bool, vector<Expr>>> called_funcs, bool inlined){
        
        vector<Expr> pure_args = func.definition().args();
        vector<Expr> def_args = def.args();
         
        if(!inlined){
            std::map<string, Expr> replacer;
            int i = 0;
            for(auto const &a : pure_args){
                const Variable* var = a.as<Variable>();
                internal_assert(var);
                replacer[var->name] = def_args[i];
                i++;
            }

            int j = 0;
            for(auto const &ann : def.annotations()){
                def.annotations()[j] = substitute(replacer, ann);
                j++;
            }

            // Add our own write permission, if we are not inlined
            Expr call = Call::make(func, def_args);
            def.add_annotation(Permission::make(AnnotationType::Context, make_bool(true), call, Frac::make(1, 1), {}));
        } else {
            // Clear all the annotations added, since we use the definition of inline functions, so we don't have to prove things
            def.annotations().clear();
        }

        // Now check all called functions in our definitions
        for (auto const &called_func : called_funcs) {
            Function called;
            vector<Expr> args;
            bool inlined_called;
            std::tie(called, inlined_called, args) = called_func;
            debug(3) << "Function '" << func.name() << "' calls '" << called.name() << "'\n";

            //Add read permissions for array accesses
            if(!inlined_called){
                Expr read_call = Call::make(called, args);
                def.add_annotation(Permission::make(AnnotationType::Context, make_bool(true), read_call, ReadPerm::make(), {}));
            }

            // Check the annotations, which we can turn into require annotations.
            for (auto const &ann : called.func_annotations()) {
                // We need to replace the definition arguments, with the called arguments.
                std::map<string, Expr> replacer;
                int i = 0;
                for(auto const &a : called.definition().args()){
                    const Variable* var = a.as<Variable>();
                    internal_assert(var);
                    replacer[var->name] = args[i];
                    i++;
                }

                Annotation new_ann = simplify(AnnExprToRequire().mutate(substitute(replacer, ann)));
                debug(4) << "Old annotation '" << ann << "'\n";
                debug(4) << "New annotation '" << new_ann << "'\n";
                const AnnExpr* ae = new_ann.as<AnnExpr>();
                if(ae){
                    if(!is_const_true(ae->condition))
                        def.add_annotation(new_ann);
                }
                else {
                    // A permission annotation
                    def.add_annotation(new_ann);
                }
            }
        }

        // Just add all the requirements we gathered
        if(inlined){
            for(auto &ann: def.annotations()){
                func.add_func_annotation(ann);
            }
            return;
        }

        // Check if the pure args and def args are the same, then it replaces everything that came before
        bool same = true;
        Expr condition;
         for(unsigned long i = 0; i<def_args.size(); i++)
            if(!equal(pure_args[i], def_args[i])){
                if(same){
                    same = false;
                    condition = EQ::make(pure_args[i], def_args[i]);
                } else {
                    condition = And::make(condition, EQ::make(pure_args[i], def_args[i]) );
                }
            }

        
        if(same){
            // If same, clear out the old annotations, they are not valid anymore
            func.clear_func_annotations();
        } else {
            // Else first copy the old annotations and we place a condition around them.
            vector<Annotation> old_annotations = func.func_annotations();
            func.clear_func_annotations();
            Expr not_condition = Not::make(condition);
            for(auto const &ann :old_annotations)
                func.add_func_annotation(add_antecedent(not_condition, ann));
        }

        // Check if we have any reduction vars. This means we only want ensures for the last step
        // of the reduction for_loop as loop_invariant, and the ensures is just passed to the whole function
        Expr rvar_condition;
        bool has_rvar = false;
        for (const ReductionVariable &rv : def.schedule().rvars()) {
            Expr new_cond = EQ::make(Variable::make(Int(32), rv.var), rv.min+rv.extent);
            if(has_rvar){
                rvar_condition = And::make(rvar_condition, new_cond);
            } else {
                rvar_condition = new_cond;
                has_rvar = true;
            }
        }

        // Now add our own ensure expression definitions to the function annotation
        for (auto &ann : def.annotations()){
        //for (auto it = def.annotations().begin(); it != def.annotations().end(); it++)
            IsEnsureExpressionWithCall is_ensure = IsEnsureExpressionWithCall(func.name());
            ann.accept(&is_ensure);
            // Making sure we only have ensure expression annotations (not permissions)
            if(is_ensure.has_function_call){
                //First check if the args of called functions are the same as the pure args or definition args
                for(auto &called_args: is_ensure.function_call_args)
                    for(unsigned long i = 0; i<called_args.size(); i++)
                        // Either has pure args, or can have def_args if is not an rvar is present
                        if(!equal(called_args[i], pure_args[i])){
                            if(has_rvar){
                                user_error << "The ensure annotation needs to have the same parameters as the pure function definition (since it contains a reduction):" << ann << "\n";
                            } else if(!equal(called_args[i], def_args[i])){
                                user_error << "The ensure annotation needs to have the same parameters as the function definition:" << ann << "\n";
                            }
                        }
                            
                if(same)
                    func.add_func_annotation(ann);
                else
                    func.add_func_annotation(add_antecedent(condition, ann));
                
                if(has_rvar){
                    ann = add_antecedent(rvar_condition, ann);
                }
            }
        }
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
        if(wrapper != nullptr){
            //The function was a wrapper, first visit the called function
            map<string, Function>::iterator called_func_it = env.find(wrapper->name);
            if (called_func_it == env.end())
                internal_error << "Function " << wrapper->name << "was called, but could not find a definition for it";
            Function called_func = called_func_it->second;

            //TODO: maybe this is allowed.
            if (inlined)
                internal_error << "Function " << func.name() << "was called as wrapper function, but is inlined, which we do not support";
            
            if (called_func.schedule().compute_level().is_inlined() && called_func.can_be_inlined())
                internal_error << "Function " << called_func.name() << "was called as wrapped function, but is inlined, which we do not support";

            add_function_annotations(called_func);
            // Now add the correct definitions
            vector<tuple<Function, bool, vector<Expr>>> call;
            call.emplace_back(called_func, false, wrapper->args);
            add_definition_annotations(func, def, call, false);

            // We add anotations that proved something about the called function, and prove the same thing
            // but now for the new wrapper_name function
            for(Annotation a : called_func.func_annotations()){
                ReplaceEnsureFunctionCall replace_call = ReplaceEnsureFunctionCall(called_func.name(), name);
                Annotation new_a = replace_call.mutate(a);
                func.add_func_annotation(new_a);
                func.add_annotation(new_a);
            }
            // We are done with this function
            busy_processing.erase(name);
            processed_functions.emplace(name);
            func.sort_annotations();
            debug(3) << "Processed function " << func.name() << "\n"
              << func << "\n";
            return;
        }

        FindFunctionCalls calls = FindFunctionCalls(name, env);
        def.accept(&calls);

        for(auto &called_f: calls.called_funcs) add_function_annotations(std::get<0>(called_f));

        add_definition_annotations(func, def, calls.called_funcs, inlined);

        // Add update definitions annotations
        for(auto &update :func.updates()){
            FindFunctionCalls calls = FindFunctionCalls(name, env);
            update.accept(&calls);
            for(auto &called_f: calls.called_funcs) add_function_annotations(std::get<0>(called_f));
            add_self_reference_annotations(func, update, calls.self_references);
            add_definition_annotations(func, update, calls.called_funcs, inlined);
        }

        busy_processing.erase(name);
        processed_functions.emplace(name);
        func.sort_annotations();
        debug(3) << "Processed function " << func.name() << "\n"
                 << func << "\n";
    }
    
public:
    void add_automatic_annotations(){
        for (auto &iter : env) {
            Function &func = iter.second;
            add_function_annotations(func);
        }
    }

    AutomaticAnnotations(map<string, Function> &e) : env(e) {};
    
};

void add_automatic_annotations(map<string, Function> &env) {
    AutomaticAnnotations aa = AutomaticAnnotations(env);
    aa.add_automatic_annotations();
}

}  // namespace Internal
}  // namespace Halide

