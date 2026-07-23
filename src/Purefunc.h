#ifndef HALIDE_PUREFUNC_H
#define HALIDE_PUREFUNC_H

/** \file
 *
 * defines purefunc
 */

#include <string>
#include <vector>
#include <map>
#include <type_traits>
#include <utility>

#include "Expr.h"
#include "Type.h"
#include "Var.h"
#include "Seq.h"

namespace Halide {

class Purefunc;

class PurefuncRef {
public:
    PurefuncRef();
    PurefuncRef(Purefunc *pfunc, const std::vector<Expr> &args);

    //overload "=" to define the purefunc easier.
    PurefuncRef &operator=(const Expr &rhs);

    operator Expr() const;

    //access to the underlying purefunc.
    Purefunc *purefunc() const { return pfunc; }
    const std::vector<Expr> &arguments() const { return args; }

private:
    Purefunc *pfunc;
    std::vector<Expr> args;
};

class Purefunc {
public:
    //Construct an unnamed pure function.
    Purefunc();

    //Construct a named pure function.
    explicit Purefunc(const std::string &name);
    explicit Purefunc(const char *name);

    // Construct a named pure function with an explicit return type.
    // This is required if you want to use the Purefunc before its body is defined
    explicit Purefunc(Type return_type, const std::string &name);

    // Explicitly set/lock the return type. Required for recursion.
    void set_return_type(Type t);
    bool has_return_type() const { return has_return_type_; }
    Type return_type() const { return return_type_; }

    //Get the name.
    const std::string &name() const { return name_; }

    //Return whether this pure function has been defined.
    bool defined() const { return defined_; }

    //Return whether this pure function is (directly) recursive, i.e. its body
    //contains a call to itself by name. This is computed when define() is called.
    bool is_recursive() const { return recursive_; }

    // ---- Explicit PVL signature (scalars + sequences) ----
    // If not set, PVLPrinter falls back to printing scalar Variable args from the LHS.
    enum class SigKind { Scalar, Seq };
    struct SigArg {
        SigKind kind{SigKind::Scalar};
        std::string name;
        Type scalar_type;
        Type elem_type;
        int dims{1};       // number of dimensions (1 = seq<T>, 2 = seq<seq<T> >, ...)
        std::string len_name;
    };

    Purefunc &scalar_arg(Type t, const std::string &name);
    Purefunc &seq_arg(Type elem_type, const std::string &name, int dims = 1, const std::string &len_name = "");
    Purefunc &seq_arg(const Seq &s, const std::string &param_name = "");
    bool has_explicit_signature() const { return sig_explicit_; }
    const std::vector<SigArg> &signature() const { return sig_args_; }

    // ---- annotations (requires/ensures/etc.) ----
    Purefunc &annotate(const Annotation &ann);
    Purefunc &requires(const Expr &condition);
    Purefunc &ensures(const Expr &condition);
    Purefunc &context(const Expr &condition);
    Purefunc &invariant(const Expr &condition);
    // Decreases measure for recursive Purefunc
    Purefunc &decreases(const Expr &measure);
    bool has_decreases() const { return has_decreases_; }
    const Expr &decreases_measure() const { return decreases_measure_; }

    const std::vector<Annotation> &annotations() const { return annotations_; }

    //Get the argument(s) of the definition (LHS).
    const std::vector<Expr> &args() const { return args_; }

    //Get the body of the definition (RHS).
    const Expr &body() const { return body_; }

    //Define the pure function with a given argument and body.
    void define(const std::vector<Expr> &args, const Expr &value);

    // Build an Expr that represents a call to this Purefunc with given args.
    Expr call(const std::vector<Expr> &args) const;

    // Call-operator used for defining the Purefunc:
    //   pf(x, y) = x + y;
    PurefuncRef operator()(const std::vector<Expr> &args);

    // Zero-argument call, e.g. pf()
    PurefuncRef operator()() { return PurefuncRef(this, std::vector<Expr>()); }

    // Convenience call: accepts any mix of Seq and Expr-constructible
    // arguments in any position. Seq arguments are passed via their handle().
    template<typename First, typename... Rest,
             typename = typename std::enable_if<
                 !std::is_same<typename std::decay<First>::type,
                               std::vector<Expr>>::value>::type>
    PurefuncRef operator()(First &&first, Rest &&...rest) {
        std::vector<Expr> args;
        args.reserve(1 + sizeof...(rest));
        args.push_back(to_pf_arg(std::forward<First>(first)));
        (args.push_back(to_pf_arg(std::forward<Rest>(rest))), ...);
        return PurefuncRef(this, args);
    }

private:
    static Expr to_pf_arg(const Seq &s) { return s.handle(); }
    template<typename T,
             typename = typename std::enable_if<
                 !std::is_same<typename std::decay<T>::type, Seq>::value>::type>
    static Expr to_pf_arg(T &&x) { return Expr(std::forward<T>(x)); }

    std::string name_;
    bool defined_;
    bool recursive_{false};
    std::vector<Expr> args_;
    Expr body_;
    Type return_type_;
    bool has_return_type_{false};
    Expr decreases_measure_;
    bool has_decreases_{false};

    std::vector<Annotation> annotations_;

    std::vector<SigArg> sig_args_;
    bool sig_explicit_{false};
};

// Global registry of user-declared pure functions, indexed by name.
const std::map<std::string, Purefunc *> &purefunc_registry();

}  // namespace Halide

#endif  // HALIDE_PUREFUNC_H
