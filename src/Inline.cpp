#include <set>

#include "CSE.h"
#include "Debug.h"
#include "ExternFuncArgument.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRPrinter.h"
#include "Inline.h"
#include "Qualify.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

// Sanity check that this is a reasonable function to inline
void validate_schedule_inlined_function(Function f) {
    const FuncSchedule &func_s = f.schedule();
    const StageSchedule &stage_s = f.definition().schedule();

    if (!func_s.store_level().is_inlined()) {
        user_error << "Function " << f.name() << " is scheduled to be computed inline, "
                   << "but is not scheduled to be stored inline. A storage schedule "
                   << "is meaningless for functions computed inline.\n";
    }

    // Inlining is allowed only if there is no specialization.
    user_assert(f.definition().specializations().empty())
        << "Function " << f.name() << " is scheduled inline, so it"
        << " must not have any specializations. Specialize on the"
        << " scheduled function instead.\n";

    if (func_s.memoized()) {
        user_error << "Cannot memoize function "
                   << f.name() << " because the function is scheduled inline.\n";
    }

    for (size_t i = 0; i < stage_s.dims().size(); i++) {
        Dim d = stage_s.dims()[i];
        if (d.is_unordered_parallel()) {
            user_error << "Cannot parallelize dimension "
                       << d.var << " of function "
                       << f.name() << " because the function is scheduled inline.\n";
        } else if (d.for_type == ForType::Unrolled) {
            user_error << "Cannot unroll dimension "
                       << d.var << " of function "
                       << f.name() << " because the function is scheduled inline.\n";
        } else if (d.for_type == ForType::Vectorized) {
            user_error << "Cannot vectorize dimension "
                       << d.var << " of function "
                       << f.name() << " because the function is scheduled inline.\n";
        }
    }

    for (size_t i = 0; i < stage_s.splits().size(); i++) {
        if (stage_s.splits()[i].is_rename()) {
            user_warning << "It is meaningless to rename variable "
                         << stage_s.splits()[i].old_var << " of function "
                         << f.name() << " to " << stage_s.splits()[i].outer
                         << " because " << f.name() << " is scheduled inline.\n";
        } else if (stage_s.splits()[i].is_fuse()) {
            user_warning << "It is meaningless to fuse variables "
                         << stage_s.splits()[i].inner << " and " << stage_s.splits()[i].outer
                         << " because " << f.name() << " is scheduled inline.\n";
        } else {
            user_warning << "It is meaningless to split variable "
                         << stage_s.splits()[i].old_var << " of function "
                         << f.name() << " into "
                         << stage_s.splits()[i].outer << " * "
                         << stage_s.splits()[i].factor << " + "
                         << stage_s.splits()[i].inner << " because "
                         << f.name() << " is scheduled inline.\n";
        }
    }

    for (size_t i = 0; i < func_s.bounds().size(); i++) {
        if (func_s.bounds()[i].min.defined()) {
            user_warning << "It is meaningless to bound dimension "
                         << func_s.bounds()[i].var << " of function "
                         << f.name() << " to be within ["
                         << func_s.bounds()[i].min << ", "
                         << func_s.bounds()[i].extent << "] because the function is scheduled inline.\n";
        } else if (func_s.bounds()[i].modulus.defined()) {
            user_warning << "It is meaningless to align the bounds of dimension "
                         << func_s.bounds()[i].var << " of function "
                         << f.name() << " to have modulus/remainder ["
                         << func_s.bounds()[i].modulus << ", "
                         << func_s.bounds()[i].remainder << "] because the function is scheduled inline.\n";
        }
    }
}

class SubstituteWithGhostCall : public IRMutator {
    const std::map<string, Expr> &replace;
    const std::map<string, Expr> &replace_ghost;
    Scope<> hidden;
    bool in_ghost = false;

    Expr find_replacement(const string &s){
        if(in_ghost){
            return find_replacement_h(s, replace_ghost);
        } else {
            return find_replacement_h(s, replace);
        }
    }

    Expr find_replacement_h(const string &s, const std::map<string, Expr> &replace) {
        std::map<string, Expr>::const_iterator iter = replace.find(s);
        if (iter != replace.end() && !hidden.contains(s)) {
            return iter->second;
        } else {
            return Expr();
        }
    }

public:
    SubstituteWithGhostCall(const std::map<string, Expr> &m, const std::map<string, Expr> &ghost_m)
        : replace(m), replace_ghost(ghost_m) {
    }

    using IRMutator::visit;

    Expr visit(const Variable *v) override {
        Expr r = find_replacement(v->name);
        if (r.defined()) {
            return r;
        } else {
            return v;
        }
    }

    Expr visit(const Let *op) override {
        Expr new_value = mutate(op->value);
        hidden.push(op->name);
        Expr new_body = mutate(op->body);
        hidden.pop(op->name);

        if (new_value.same_as(op->value) &&
            new_body.same_as(op->body)) {
            return op;
        } else {
            return Let::make(op->name, new_value, new_body);
        }
    }

    Expr visit(const Call *op) override {
        if(op->call_type == Call::Halide || op->call_type == Call::Image){
            Expr new_call = IRMutator::visit(op);

            vector<Expr> ghost_args;
            ghost_args.emplace_back(new_call);
            bool old_ghost = in_ghost;
            in_ghost = true;
            for(const auto &a: op->args){
                Expr new_a = mutate(a);
                ghost_args.emplace_back(new_a);
            }
            in_ghost = old_ghost;
            return Call::make(op->type, Call::ghost_args, {ghost_args}, Call::Intrinsic);
        } else {
            return IRMutator::visit(op);
        }
    }
};

class Inliner : public IRMutator {
    using IRMutator::visit;

    Function func;
    bool add_ghost;
    vector<Expr> ghost_args;

    Expr visit(const Call *op) override {
        if (op->name == func.name()) {

            // Mutate the args
            vector<Expr> args(op->args.size());
            for (size_t i = 0; i < args.size(); i++) {
                args[i] = mutate(op->args[i]);
            }
            // Grab the body
            Expr body = qualify(func.name() + ".", func.values()[op->value_index]);

            const vector<string> func_args = func.args();

            // Bind the args using Let nodes
            internal_assert(args.size() == func_args.size());
            internal_assert(!add_ghost || ghost_args.size() == args.size());
            std::map<string, Expr> replace;
            std::map<string, Expr> replace_ghost;
            for (size_t i = 0; i < args.size(); i++) {
                string name = func.name() + "." + func_args[i];
                if(add_ghost){
                    replace_ghost[name] = ghost_args[i];
                }
                if (is_const(args[i]) || args[i].as<Variable>()) {
                    replace[name] = args[i];
                }
            }
            if(add_ghost){
                internal_assert(ghost_args.size() == args.size());
                SubstituteWithGhostCall sub(replace, replace_ghost);
                body = sub.mutate(body);
            } else {
                body = substitute(replace, body);
            }

            for (size_t i = 0; i < args.size(); i++) {
                if (!(is_const(args[i]) || args[i].as<Variable>())) {
                    body = Let::make(func.name() + "." + func_args[i], args[i], body);
                }
            }

            found++;

            return body;

        } else if(add_ghost && op->is_intrinsic(Call::ghost_args)){
            // The call is inlined, so we need to inline the ghost args as well
            const Call *call = op->args[0].as<Call>();
            internal_assert(call);
            if(call->name != func.name()){
                return IRMutator::visit(op);
            }

            vector<Expr> ghost_args_here;
            for (size_t i = 1; i < op->args.size(); i++) {
                ghost_args_here.emplace_back(op->args[i]);
            }
            ghost_args = ghost_args_here;
            Expr result = mutate(op->args[0]);
            ghost_args = {};
            return result;
        
        } else {
            return IRMutator::visit(op);
        }
    }

    Expr visit(const Variable *op) override {
        if (op->name == func.name() + ".buffer") {
            const Call *call = func.is_wrapper();
            internal_assert(call);
            // Do a whole-image inline. Substitute the .buffer symbol
            // for the wrapped object's .buffer symbol.
            string buf_name;
            if (call->call_type == Call::Halide) {
                buf_name = call->name;
                if (Function(call->func).outputs() > 1) {
                    buf_name += "." + std::to_string(call->value_index);
                }
                buf_name += ".buffer";
                return Variable::make(type_of<halide_buffer_t *>(), buf_name);
            } else if (call->param.defined()) {
                return Variable::make(type_of<halide_buffer_t *>(), call->name + ".buffer", call->param);
            } else {
                internal_assert(call->image.defined());
                return Variable::make(type_of<halide_buffer_t *>(), call->name + ".buffer", call->image);
            }
        } else {
            return op;
        }
    }

    Stmt visit(const Provide *op) override {
        ScopedValue<int> old_found(found, 0);
        Stmt stmt = IRMutator::visit(op);

        // TODO: making this > 1 should be desirable,
        // but explodes compiletimes in some situations.
        if (found > 0) {
            stmt = common_subexpression_elimination(stmt);
        }

        return stmt;
    }

public:
    int found = 0;

    Inliner(const Function &f, bool add_ghost)
        : func(f), add_ghost(add_ghost) {
        internal_assert(f.can_be_inlined()) << "Illegal to inline " << f.name() << "\n";
        validate_schedule_inlined_function(f);
    }
};

Stmt inline_function(Stmt s, const Function &f, bool add_ghost) {
    Inliner i(f, add_ghost);
    s = i.mutate(s);
    return s;
}

Expr inline_function(Expr e, const Function &f, bool add_ghost) {
    Inliner i(f, add_ghost);
    e = i.mutate(e);
    // TODO: making this > 1 should be desirable,
    // but explodes compiletimes in some situations.
    if (i.found > 0) {
        e = common_subexpression_elimination(e);
    }
    return e;
}

// Inline all calls to 'f' inside 'caller'
void inline_function(Function caller, const Function &f) {
    Inliner i(f, false);
    caller.mutate(&i);
    if (caller.has_extern_definition()) {
        for (ExternFuncArgument &arg : caller.extern_arguments()) {
            if (arg.is_func() && arg.func.same_as(f.get_contents())) {
                const Call *call = f.is_wrapper();
                internal_assert(call);
                arg.func = call->func;
            }
        }
    }
}

}  // namespace Internal
}  // namespace Halide
