#include "Purefunc.h"

#include "Expr.h"
#include "Var.h"
#include "IRVisitor.h"
#include "IR.h"
#include "Error.h"
#include <map>

namespace Halide {

namespace {

// Detect whether an Expr contains a call to a given name.
class ContainsCallToName : public Halide::Internal::IRVisitor {
public:
    explicit ContainsCallToName(const std::string &target) : target(target) {}
    bool found{false};

private:
    const std::string target;
    using Halide::Internal::IRVisitor::visit;

    void visit(const Halide::Internal::Call *op) override {
        if (op->name == target) {
            found = true;
            return;
        }
        IRVisitor::visit(op);
    }
};

// Internal mutable registry; external code only sees it as const.
std::map<std::string, Purefunc *> &purefunc_registry_mutable() {
    static std::map<std::string, Purefunc *> reg;
    return reg;
}

// Register a named Purefunc in the global registry.
void register_purefunc(Purefunc *pf) {
    if (!pf) {
        return;
    }
    const std::string &n = pf->name();
    if (n.empty()) {
        return;
    }
    auto &reg = purefunc_registry_mutable();
    user_assert(reg.find(n) == reg.end())
        << "Purefunc \"" << n << "\" already exists.\n";
    reg[n] = pf;
}

} // anonymous namespace

//public accessor
const std::map<std::string, Purefunc *> &purefunc_registry() {
    return purefunc_registry_mutable();
}

//PurefuncRef

PurefuncRef::PurefuncRef() {
    pfunc = nullptr;
}

PurefuncRef::PurefuncRef(Purefunc *f, const std::vector<Expr> &a) {
    pfunc = f;
    args = a;
}

PurefuncRef &PurefuncRef::operator=(const Expr &rhs) {
    if (!pfunc) {
        user_error << "Attempting to define a null PurefuncRef.\n";
    }
    pfunc->define(args, rhs);
    return *this;
}

//allows using PurefuncRef as an Expr
PurefuncRef::operator Expr() const {
    if (!pfunc) {
        user_error << "Attempting to use a null PurefuncRef as Expr.\n";
    }
    return pfunc->call(args);
}

//Purefunc

Purefunc::Purefunc() {
    name_ = std::string();
    defined_ = false;
    recursive_ = false;
    has_return_type_ = false;
}

Purefunc::Purefunc(const std::string &name) {
    name_ = name;
    defined_ = false;
    recursive_ = false;
    has_return_type_ = false;
    register_purefunc(this);
}

Purefunc::Purefunc(const char *name) {
    name_ = std::string(name);
    defined_ = false;
    recursive_ = false;
    has_return_type_ = false;
    register_purefunc(this);
}

Purefunc::Purefunc(Type return_type, const std::string &name) {
    name_ = name;
    defined_ = false;
    recursive_ = false;
    return_type_ = return_type;
    has_return_type_ = true;
    register_purefunc(this);
}

void Purefunc::set_return_type(Type t) {
    if (has_return_type_) {
        user_assert(return_type_ == t)
            << "Purefunc \"" << name_ << "\" return type already set to "
            << return_type_ << ", got " << t << ".\n";
    } else {
        return_type_ = t;
        has_return_type_ = true;
    }
}

void Purefunc::define(const std::vector<Expr> &args, const Expr &value) {
    if (defined_) {
        user_error << "Purefunc \"" << name_<< "\" is already defined.\n";
    }
    args_ = args;
    body_ = value;
    defined_ = true;

    // Detect direct recursion: does the body call this function by name?
    recursive_ = false;
    if (!name_.empty()) {
        ContainsCallToName v(name_);
        body_.accept(&v);
        recursive_ = v.found;
    }

    // Infer or validate the return type.
    if (!has_return_type_) {
        return_type_ = value.type();
        has_return_type_ = true;
    } else {
        user_assert(return_type_ == value.type())
            << "Purefunc \"" << name_ << "\" return type mismatch: expected "
            << return_type_ << " but got " << value.type() << ".\n";
    }
}

Expr Purefunc::call(const std::vector<Expr> &args) const {
    // For recursion, calls can occur before define().
    // In that case, we must already know the return type.
    if (!has_return_type_) {
        user_error
            << "Error: Can't call Purefunc \"" << name_ << "\" before its return type is known.\n"
            << "For recursive Purefunc, use Purefunc(Type, name) or call set_return_type(... ) before defining it.\n";
    }

    // If we've been defined, check arguments against definition.
    if (defined_) {
        user_assert(args.size() == args_.size())
            << "Wrong number of arguments in call to Purefunc \"" << name_ << "\". Expected "
            << args_.size() << " but got " << args.size() << ".\n";
    }

    return Internal::Call::make(return_type_, name_, args, Internal::Call::Extern);
}

PurefuncRef Purefunc::operator()(const std::vector<Expr> &args) {
    return PurefuncRef(this, args);
}

// ---- signature helpers ----

//add a scalar parameter to the purefunc argument list.
Purefunc &Purefunc::scalar_arg(Type t, const std::string &name) {
    sig_explicit_ = true;
    SigArg a;
    a.kind = SigKind::Scalar;
    a.name = name;
    a.scalar_type = t;
    sig_args_.push_back(a);
    return *this;
}

//add a sequence parameter to the purefunc argument list.
Purefunc &Purefunc::seq_arg(Type elem_type, const std::string &name, int dims, const std::string &len_name) {
    sig_explicit_ = true;
    SigArg a;
    a.kind = SigKind::Seq;
    a.name = name;
    a.elem_type = elem_type;
    a.dims = dims;
    a.len_name = len_name;
    sig_args_.push_back(a);
    return *this;
}

//add a sequence parameter using metadata from an existing Seq.
Purefunc &Purefunc::seq_arg(const Seq &s, const std::string &param_name) {
    const std::string &nm = param_name.empty() ? s.name() : param_name;
    return seq_arg(s.elem_type(), nm, s.dimensions(), s.len_name());
}

// ---- annotations ----

Purefunc &Purefunc::annotate(const Annotation &ann) {
    annotations_.push_back(ann);
    return *this;
}

Purefunc &Purefunc::requires(const Expr &condition) {
    user_assert(condition.defined()) << "Purefunc::requires got undefined expression.\n";
    user_assert(condition.type().is_bool()) << "Purefunc::requires expects a boolean Expr.\n";
    return annotate(Internal::AnnExpr::make(Internal::AnnotationType::Require, condition));
}

Purefunc &Purefunc::ensures(const Expr &condition) {
    user_assert(condition.defined()) << "Purefunc::ensures got undefined expression.\n";
    user_assert(condition.type().is_bool()) << "Purefunc::ensures expects a boolean Expr.\n";
    return annotate(Internal::AnnExpr::make(Internal::AnnotationType::Ensure, condition));
}

Purefunc &Purefunc::context(const Expr &condition) {
    user_assert(condition.defined()) << "Purefunc::context got undefined expression.\n";
    user_assert(condition.type().is_bool()) << "Purefunc::context expects a boolean Expr.\n";
    return annotate(Internal::AnnExpr::make(Internal::AnnotationType::Context, condition));
}

Purefunc &Purefunc::invariant(const Expr &condition) {
    user_assert(condition.defined()) << "Purefunc::invariant got undefined expression.\n";
    user_assert(condition.type().is_bool()) << "Purefunc::invariant expects a boolean Expr.\n";
    return annotate(Internal::AnnExpr::make(Internal::AnnotationType::LoopInvariant, condition));
}

Purefunc &Purefunc::decreases(const Expr &measure) {
    user_assert(measure.defined()) << "Purefunc::decreases got undefined expression.\n";
    decreases_measure_ = measure;
    has_decreases_ = true;
    return *this;
}

}  // namespace Halide

