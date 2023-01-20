#include "FindCalls.h"

#include "ExternFuncArgument.h"
#include "Function.h"
#include "IRVisitor.h"
#include <utility>

namespace Halide {
namespace Internal {

using std::map;
using std::string;

namespace {
/* Find all the internal halide calls in an expr */
class FindCalls : public IRVisitor {
public:
    FindCalls(map<string, Parameter> &par_env) : parameter_calls(par_env) { }

    map<string, Function> calls;
    map<string, Parameter> parameter_calls;

    using IRVisitor::visit;

    void include_function(const Function &f) {
        map<string, Function>::iterator iter = calls.find(f.name());
        if (iter == calls.end()) {
            calls[f.name()] = f;
        } else {
            user_assert(iter->second.same_as(f))
                << "Can't compile a pipeline using multiple functions with the same name: "
                << f.name() << "\n";
        }
    }

    void include_parameter(const Parameter &p) {
        map<string, Parameter>::iterator iter = parameter_calls.find(p.name());
        if (iter == parameter_calls.end()) {
            parameter_calls[p.name()] = p;
        } else {
            user_assert(iter->second.same_as(p))
                << "Can't compile a pipeline using multiple parameters with the same name: "
                << p.name() << "\n";
        }
    }

    void visit(const Call *call) override {
        IRVisitor::visit(call);

        if (call->call_type == Call::Halide && call->func.defined()) {
            Function f(call->func);
            include_function(f);
        }

        if (call->call_type == Call::Image && call->param.defined()) {
            include_parameter(call->param);
        }
    }
};

void add_par(Parameter p, map<string, Parameter> &par_env) {
        map<string, Parameter>::iterator iter = par_env.find(p.name());
        if (iter == par_env.end()) {
            par_env[p.name()] = p;
        } else {
            user_assert(iter->second.same_as(p))
                << "Can't compile a pipeline using multiple parameters with the same name: "
                << p.name() << "\n";
        }
    }

void populate_environment_helper(Function f, map<string, Function> &env, map<string, Parameter> &par_env,
                                 bool recursive = true, bool include_wrappers = false) {
    map<string, Function>::const_iterator iter = env.find(f.name());
    if (iter != env.end()) {
        user_assert(iter->second.same_as(f))
            << "Can't compile a pipeline using multiple functions with same name: "
            << f.name() << "\n";
        return;
    }

    FindCalls calls(par_env);
    f.accept(&calls);
    if (f.has_extern_definition()) {
        for (const ExternFuncArgument &arg : f.extern_arguments()) {
            if (arg.is_func()) {
                Function g(arg.func);
                calls.calls[g.name()] = g;
            }
        }
    }

    if (include_wrappers) {
        for (const auto &it : f.schedule().wrappers()) {
            Function g(it.second);
            calls.calls[g.name()] = g;
        }
    }

    for(const auto &it: calls.parameter_calls){
        add_par(it.second, par_env);
    }

    if (!recursive) {
        env.insert(calls.calls.begin(), calls.calls.end());
    } else {
        env[f.name()] = f;
        for (const auto &i : calls.calls) {
            populate_environment_helper(i.second, env, par_env, recursive, include_wrappers);
        }
    }
}

}  // namespace

void populate_environment(Function f, map<string, Function> &env) {
    map<string, Parameter> par_res;
    populate_environment_helper(std::move(f), env, par_res, true, true);
}

void find_parameter_and_function_calls(Function f, map<string, Function> &env, map<string, Parameter> &par_res) {
    populate_environment_helper(std::move(f), env, par_res, true, true);
}

map<string, Function> find_transitive_calls(Function f) {
    map<string, Function> res;
    map<string, Parameter> par_res;
    populate_environment_helper(std::move(f), res, par_res, true, false);
    return res;
}

map<string, Function> find_direct_calls(Function f) {
    map<string, Function> res;
    map<string, Parameter> par_res;
    populate_environment_helper(std::move(f), res, par_res, false, false);
    return res;
}

}  // namespace Internal
}  // namespace Halide
