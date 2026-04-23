#ifndef HALIDE_SEQ_H
#define HALIDE_SEQ_H

/** \file
 *
 * immutable sequence
 */

#include <string>
#include <vector>

#include "Expr.h"
#include "Type.h"
#include "IR.h"
#include "ImageParam.h"

namespace Halide {

//for printing stuff in PVL and C
static constexpr const char *k_pvl_seq_len     = "__seq_len"; // |xs|
static constexpr const char *k_pvl_seq_at      = "__seq_at"; // x[i]
static constexpr const char *k_pvl_image_seq   = "__image_seq"; // inp_seq()
static constexpr const char *k_pvl_img_extent  = "__img_extent"; // extent

class Seq {
public:
    Seq() = default; //empty constructor

    // 1D named sequence: Seq x("x", Int(32));
    Seq(const std::string &name, Type elem_type, const std::string &len_name = "")
        : kind_(Kind::Named), name_(name), elem_type_(elem_type), dims_(1), len_(len_name) {}

    // multidimensional named sequence: Seq x("x", Int(32), 2);
    Seq(const std::string &name, Type elem_type, int dims, const std::string &len_name = "")
        : kind_(Kind::Named), name_(name), elem_type_(elem_type), dims_(dims), len_(len_name) {}

    // Sequence from ImageParam: Seq xs(imgprm);
    explicit Seq(const ImageParam &im, const std::string &len_name = "")
        : kind_(Kind::ImageView), name_(im.name()), elem_type_(im.type()), dims_(im.dimensions()), len_(len_name) {}

    //getters
    const std::string &name() const { return name_; }
    Type elem_type() const { return elem_type_; }
    const std::string &len_name() const { return len_; }
    int dimensions() const { return dims_; }

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
            return Internal::Call::make(Handle(), k_pvl_image_seq, {image_handle()}, Internal::Call::Extern); // For image view: handle is __image_seq(image_handle)
        }
    }

    // Length of the outermost dimension: PVL and C: |xs| or |inp_seq()|
    Expr len() const {
        return Internal::Call::make(Int(32), k_pvl_seq_len, {handle()}, Internal::Call::Extern);
    }

    // Length of a specific dimension.
    // dim=0: |handle|, dim=1: |handle[0]|, dim=2: |handle[0][0]|, etc.
    Expr len(int dim) const {
        Expr h = handle();
        for (int d = 0; d < dim; d++) {
            h = Internal::Call::make(Handle(), k_pvl_seq_at, {h, Expr(0)}, Internal::Call::Extern);
        }
        return Internal::Call::make(Int(32), k_pvl_seq_len, {h}, Internal::Call::Extern);
    }

    // Single-index access
    // PVL and C: xs[i] or inp_seq()[i]
    Expr operator[](const Expr &i) const {
        Type result_type = (dims_ > 1) ? Handle() : elem_type_;
        return Internal::Call::make(result_type, k_pvl_seq_at, {handle(), i}, Internal::Call::Extern);
    }

    // Multi-index access for multidimensional sequences.
    // seq(i, j) builds __seq_at(__seq_at(handle, i), j) with correct types.
    Expr operator()(const std::vector<Expr> &indices) const {
        user_assert(!indices.empty())
            << "Seq::operator() requires at least one index.\n";
        user_assert((int)indices.size() <= dims_)
            << "Seq::operator() got " << indices.size() << " indices but sequence \""
            << name_ << "\" has only " << dims_ << " dimension(s).\n";

        Expr h = handle();
        for (size_t d = 0; d < indices.size(); d++) {
            int remaining = dims_ - (int)d - 1;
            Type result_type = (remaining > 0) ? Handle() : elem_type_;
            h = Internal::Call::make(result_type, k_pvl_seq_at, {h, indices[d]}, Internal::Call::Extern);
        }
        return h;
    }

    // Convenience overloads for multi-index access.
    Expr operator()(const Expr &i) const {
        return (*this)(std::vector<Expr>{i});
    }
    template<typename... Args>
    Expr operator()(const Expr &i0, const Expr &i1, Args &&...rest) const {
        std::vector<Expr> indices;
        indices.reserve(2 + sizeof...(rest));
        indices.push_back(i0);
        indices.push_back(i1);
        (indices.push_back(Expr(std::forward<Args>(rest))), ...);
        return (*this)(indices);
    }

    // PVL and C: extent(inp, d)  (only for Seq(ImageParam))
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
    int dims_{1};
    std::string len_; //not being used rn
};

}  // namespace Halide

#endif  // HALIDE_SEQ_H