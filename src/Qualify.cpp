#include "Qualify.h"
#include "IRMutator.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

namespace {

// Prefix all names in an expression with some string.
class QualifyExpr : public IRMutator {
    using IRMutator::visit;

    const string &prefix;

    Expr visit(const Variable *v) override {
        if (v->param.defined()) {
            return v;
        } else {
            return Variable::make(v->type, prefix + v->name, v->reduction_domain);
        }
    }
    Expr visit(const Let *op) override {
        Expr value = mutate(op->value);
        Expr body = mutate(op->body);
        return Let::make(prefix + op->name, value, body);
    }

    Expr visit(const Forall *op) override {
        Expr select = mutate(op->select);
        Expr main = mutate(op->main);
        vector<string> new_vars;
        for(const string &v: op->vars){
            new_vars.emplace_back(prefix + v);
        }
    
        return Forall::make(new_vars, std::move(select), std::move(main));
    }

public:
    QualifyExpr(const string &p)
        : prefix(p) {
    }
};

}  // namespace

Expr qualify(const string &prefix, const Expr &value) {
    QualifyExpr q(prefix);
    return q.mutate(value);
}

vector<Annotation> qualify(const string &prefix, const vector<Annotation> &anns){
    vector<Annotation> results;
    QualifyExpr q(prefix);
    for(const Annotation &a : anns){
        results.emplace_back(q.mutate(a));
    }
    
    return results;
}

}  // namespace Internal
}  // namespace Halide
