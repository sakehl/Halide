#ifndef HALIDE_ADD_PARAMETER_ANNOTATIONS_H
#define HALIDE_ADD_PARAMETER_ANNOTATIONS_H

/** \file
 * Defines the lowering pass that adds annotations throughtout the program that are associated with parameters.
 */

#include "Expr.h"
#include "Function.h"

namespace Halide {

struct Target;

namespace Internal {

std::pair<Stmt, std::vector<Annotation>> add_pipeline_annotations(const Stmt &stmt, std::vector<Parameter> input, std::vector<Parameter> output, std::vector<Annotation> pipeline_anns);

}  // namespace Internal
}  // namespace Halide

#endif
