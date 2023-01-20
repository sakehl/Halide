#ifndef HALIDE_ADD_PARAMETER_ANNOTATIONS_H
#define HALIDE_ADD_PARAMETER_ANNOTATIONS_H

/** \file
 * Defines the lowering pass that adds annotations throughtout the program that are associated with parameters.
 */

#include "Expr.h"
#include "Function.h"

using std::pair;
using std::vector;

namespace Halide {

struct Target;

namespace Internal {

pair<Stmt, vector<Annotation>> add_parameter_annotations(const Stmt &stmt, vector<Parameter> input, vector<Parameter> output);

}  // namespace Internal
}  // namespace Halide

#endif
