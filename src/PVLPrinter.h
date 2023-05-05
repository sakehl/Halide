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

using std::map;
using std::string;
using std::vector;

/** This class emits PVL code equivalent to the halide algorithm language (Front-End).
 * It's mostly the same as an IRPrinter, but we have the hande functions differently
 */
class PVLPrinter : public IRPrinter {
public:

    /** Initialize a PVL code generator pointing at a particular output
     * stream (e.g. a file, or std::cout) */
    PVLPrinter(std::ostream &dest);

    void print_pipeline(const vector<Annotation> &anns);

    void print_func(Function f);

    void print_buffer(Parameter p);
private:
    bool in_annotations;
    bool in_reduction;
    bool buffer_annotation;

    // The function name we are currently translating
    string func_name;
    // The name of the previous definition of the function
    string prev_def_name;
    // The pure arguments of the function we are translating
    vector<string> pure_args;
    // The definition arguments of the current function
    vector<Expr> def_args;
    // The reduction variables present in the current function
    vector<string> rvars;

    map<string, Parameter> parameter_map;

    using IRPrinter::visit;

    void visit(const Call *) override;
    void visit(const Cast *) override;
    void visit(const Div *) override;
    void visit(const Let *) override;
    void visit(const Forall *) override;
    void visit(const Exists *) override;
    void visit(const Variable *) override;
    void visit(const UIntImm * ) override;
    void visit(const Select * ) override;

    void visit(const AnnExpr *) override;
    void visit(const Permission *) override;

    bool call_correct(const Call *);

    bool ends_on_dimension(string name);

    void print_buffer_members(Parameter p);
    
    void print_def(Definition def, vector<string> original_args, vector<Type> output_types, string func_name, string old_func_name);

    void print_lhs_def(vector<string> original_args, vector<Type> output_types, string func_name);

    void print_red_func(Definition def, vector<string> original_args, vector<Expr> different_args, vector<Type> output_types,
        string func_name, string old_func_name);

    void print_ann(const vector<Annotation> &anns, bool has_reduction = false);

    void print_reduction_ann(const vector<Annotation> &anns, const vector<ReductionVariable> &rvars);

    void print_type(const Type &type);

    
};

}
}

#endif
