#ifndef HALIDE_SEQ_H
#define HALIDE_SEQ_H

/** \file
 *
 * immutable sequence.
 */

#include <string>

#include "Expr.h"
#include "Type.h"
#include "IR.h"
#include "ImageParam.h"

namespace Halide {

//for printing stuff in PVL
//look here -> "void PVLPrinter::visit(const Call *op) {" in PVL printer.cpp
static constexpr const char *k_pvl_seq_len     = "__pvl_seq_len"; // |xs|
static constexpr const char *k_pvl_seq_at      = "__pvl_seq_at"; // x[i]
static constexpr const char *k_pvl_image_seq   = "__pvl_image_seq"; // inp_seq()
static constexpr const char *k_pvl_img_extent  = "__pvl_img_extent"; // extent

class Seq {
public:
    Seq() = default; //empty constructor

    // example: Seq x("x", Int(32));
    Seq(const std::string &name, Type elem_type, const std::string &len_name = "")
        : kind_(Kind::Named), name_(name), elem_type_(elem_type), len_name_(len_name) {}

    // example: Seq xs(imgprm);
    explicit Seq(const ImageParam &im, const std::string &len_name = "")
        : kind_(Kind::ImageView), name_(im.name()), elem_type_(im.type()), len_name_(len_name) {}

    //getters
    const std::string &name() const { return name_; }
    Type elem_type() const { return elem_type_; }
    const std::string &len_name() const { return len_name_; }

    //Returns true if constructed from ImageParam, false if it is a normal named sequence
    bool is_image_view() const { return kind_ == Kind::ImageView; }

    // A symbolic handle Expr for the image/buffer name (used in PVL printing)
    Expr image_handle() const {
        return Internal::Variable::make(Handle(), name_);
    }

    // Underlying symbolic handle Expr used to pass this sequence to Purefunc calls.
    Expr handle() const {
        if (kind_ == Kind::Named) {
            return Internal::Variable::make(Handle(), name_); // For named seq: handle is a Variable(name)
        }
        else {
            return Internal::Call::make(Handle(), k_pvl_image_seq, {image_handle()}, Internal::Call::Extern); // For image view: handle is __pvl_image_seq(image_handle)
        }
    }

    // PVL: |xs| or |inp_seq()|
    Expr len() const {
        return Internal::Call::make(Int(32), k_pvl_seq_len, {handle()}, Internal::Call::Extern);
    }

    // PVL: xs[i] or inp_seq()[i]
    Expr operator[](const Expr &i) const {
        return Internal::Call::make(elem_type_, k_pvl_seq_at, {handle(), i}, Internal::Call::Extern);
    }

    // PVL: extent(inp, d)  (only for Seq(ImageParam))
    Expr extent(const Expr &d) const {
        user_assert(kind_ == Kind::ImageView)
            << "Seq::extent() is only valid for Seq constructed from ImageParam.\n";
        return Internal::Call::make(Int(32), k_pvl_img_extent, {image_handle(), d}, Internal::Call::Extern);
    }

private:
    enum class Kind { Named, ImageView };
    Kind kind_{Kind::Named};

    std::string name_;
    Type elem_type_;
    std::string len_name_; //not being used rn
};

}  // namespace Halide

#endif  // HALIDE_SEQ_H