#ifndef HALIDE_PVLPrinter_H
#define HALIDE_PVLPrinter_H

/** \file
 *
 * Defines an IRPrinter that emits PVL code equivalent to the halide algorithm language (Front-End)
 */

#include "IRPrinter.h"
#include "OutputImageParam.h"

namespace Halide {

namespace Internal {

Expr add_trigger(Expr e, const std::string &func, std::vector<Expr> &def_args, std::vector<Expr> &pure_args, bool buffer_annotation);

/** This class emits PVL code equivalent to the halide algorithm language (Front-End).
 * It's mostly the same as an IRPrinter, but we have the hande functions differently
 */
class PVLPrinter : public IRPrinter {
public:

    /** Initialize a PVL code generator pointing at a particular output
     * stream (e.g. a file, or std::cout) */
    PVLPrinter(std::ostream &dest);

    void print_pipeline(const std::vector<Annotation> &anns);

    void print_func(Function f);

    void print_buffer(Parameter p, bool is_input);
private:
    bool in_annotations;
    bool in_reduction;
    bool buffer_annotation;
    Scope<> reduction_vars;

    // The function name we are currently translating
    std::string func_name;
    // The name of the previous definition of the function
    std::string prev_def_name;
    // The pure arguments of the function we are translating
    std::vector<std::string> pure_args;
    // The definition arguments of the current function
    std::vector<Expr> def_args;
    // The reduction variables present in the current function
    std::vector<std::string> rvars;

    std::map<std::string, Parameter> parameter_map;

    using IRPrinter::visit;

    void visit(const Call *) override;
    void visit(const Cast *) override;
    void visit(const Div *) override;
    void visit(const Mod *) override;
    void visit(const Let *) override;
    void visit(const Forall *) override;
    void visit(const Exists *) override;
    void visit(const Variable *) override;
    void visit(const UIntImm * ) override;
    void visit(const Select * ) override;

    void visit(const AnnExpr *) override;

    bool call_correct(const Call *);

    bool ends_on_dimension(std::string name);

    void print_buffer_members(Parameter p);
    
    void print_def(Definition def, std::vector<std::string> original_args, std::vector<Type> output_types, std::string func_name, std::string old_func_name);

    void print_lhs_def(std::vector<std::string> original_args, std::vector<Type> output_types, std::string func_name);

    void print_red_func(Definition def, std::vector<std::string> original_args, std::vector<Expr> different_args, std::vector<Type> output_types,
        std::string func_name, std::string old_func_name);

    void print_ann(const std::vector<Annotation> &anns, bool has_reduction = false);

    void print_reduction_ann(const std::vector<Annotation> &anns, const std::vector<ReductionVariable> &red_vars);

    void print_type(const Type &type);

    
};

}
}

#endif
