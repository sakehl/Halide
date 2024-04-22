#ifndef AUTOMATE_ANNOTATIONS_H
#define AUTOMATE_ANNOTATIONS_H

/** \file
 *
 * Defines pass that try to add annotations automatically to a function.
 */

#include <map>
#include <string>
#include <vector>

#include "Expr.h"
#include "Parameter.h"

namespace Halide {
namespace Internal {

class Function;

/** Automatically add the following annotations automatically to a function:
 * - If calling another function, which is not inlined, take over the ensure conditions.
 * - Permission for reading and writing to arrays */
void add_automatic_annotations(std::map<std::string, Function> &env, std::vector<Function> &output_funcs);

/** Define definitions for predicates, but complete and partial
*/
std::string define_predicates(std::map<std::string, Function> &env, std::vector<Function> &output_funcs, std::vector<Parameter> & input_buffers);

}  // namespace Internal
}  // namespace Halide

#endif
