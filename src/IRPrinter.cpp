#include <iostream>
#include <sstream>

#include "IRPrinter.h"
#include "IRVisitor.h"

#include "AssociativeOpsTable.h"
#include "Associativity.h"
#include "IROperator.h"
#include "Module.h"
#include "Target.h"

namespace Halide {

using std::ostream;
using std::ostringstream;
using std::string;
using std::vector;

ostream &operator<<(ostream &out, const Type &type) {
    switch (type.code()) {
    case Type::Int:
        out << "int";
        break;
    case Type::UInt:
        out << "uint";
        break;
    case Type::Float:
        out << "float";
        break;
    case Type::Handle:
        if (type.handle_type) {
            out << "(" << type.handle_type->inner_name.name << " *)";
        } else {
            out << "(void *)";
        }
        break;
    case Type::BFloat:
        out << "bfloat";
        break;
    case Type::Resource:
        out << "resource";
        break;
    }
    if (!type.is_handle()) {
        out << type.bits();
    }
    if (type.lanes() > 1) {
        out << "x" << type.lanes();
    }
    return out;
}

ostream &operator<<(ostream &stream, const Expr &ir) {
    if (!ir.defined()) {
        stream << "(undefined)";
    } else {
        Internal::IRPrinter p(stream);
        p.print(ir);
    }
    return stream;
}

ostream &operator<<(ostream &stream, const Buffer<> &buffer) {
    return stream << "buffer " << buffer.name() << " = {...}\n";
}

ostream &operator<<(ostream &stream, const Module &m) {
    for (const auto &s : m.submodules()) {
        stream << s << "\n";
    }

    stream << "module name=" << m.name() << ", target=" << m.target().to_string() << "\n";
    for (const auto &b : m.buffers()) {
        stream << b << "\n";
    }
    for (const auto &f : m.functions()) {
        stream << f << "\n";
    }
    return stream;
}

ostream &operator<<(ostream &out, const DeviceAPI &api) {
    switch (api) {
    case DeviceAPI::Host:
    case DeviceAPI::None:
        break;
    case DeviceAPI::Default_GPU:
        out << "<Default_GPU>";
        break;
    case DeviceAPI::CUDA:
        out << "<CUDA>";
        break;
    case DeviceAPI::OpenCL:
        out << "<OpenCL>";
        break;
    case DeviceAPI::OpenGLCompute:
        out << "<OpenGLCompute>";
        break;
    case DeviceAPI::Metal:
        out << "<Metal>";
        break;
    case DeviceAPI::Hexagon:
        out << "<Hexagon>";
        break;
    case DeviceAPI::HexagonDma:
        out << "<HexagonDma>";
        break;
    case DeviceAPI::D3D12Compute:
        out << "<D3D12Compute>";
        break;
    }
    return out;
}

std::ostream &operator<<(std::ostream &out, const MemoryType &t) {
    switch (t) {
    case MemoryType::Auto:
        out << "Auto";
        break;
    case MemoryType::Heap:
        out << "Heap";
        break;
    case MemoryType::Stack:
        out << "Stack";
        break;
    case MemoryType::Register:
        out << "Register";
        break;
    case MemoryType::GPUShared:
        out << "GPUShared";
        break;
    case MemoryType::GPUTexture:
        out << "GPUTexture";
        break;
    case MemoryType::LockedCache:
        out << "LockedCache";
        break;
    case MemoryType::VTCM:
        out << "VTCM";
        break;
    }
    return out;
}

std::ostream &operator<<(std::ostream &out, const TailStrategy &t) {
    switch (t) {
    case TailStrategy::Auto:
        out << "Auto";
        break;
    case TailStrategy::GuardWithIf:
        out << "GuardWithIf";
        break;
    case TailStrategy::ShiftInwards:
        out << "ShiftInwards";
        break;
    case TailStrategy::RoundUp:
        out << "RoundUp";
        break;
    }
    return out;
}

ostream &operator<<(ostream &stream, const LoopLevel &loop_level) {
    return stream << "loop_level("
                  << (loop_level.defined() ? loop_level.to_string() : "undefined")
                  << ")";
}

ostream &operator<<(ostream &stream, const Target &target) {
    return stream << "target(" << target.to_string() << ")";
}

namespace Internal {

class Precedence : public IRVisitor {
public:
    int precedence = -1;
    bool left_associative = false;
    bool right_associative = false;
    bool associative = false;

    static int get_precedence(const Expr &e) {
        Precedence p;
        e.accept(&p);
        return p.precedence;
    }

    using IRVisitor::visit;

    void visit(const IntImm *) override {
        precedence = 2;
    }
    void visit(const UIntImm *) override {
        precedence = 2;
    }
    void visit(const FloatImm *) override {
        precedence = 1;
    }
    void visit(const StringImm *) override {
        precedence = 1;
    }
    void visit(const ReadPerm *) override {
        precedence = 1;
    }
    void visit(const Cast *) override {
        precedence = 2;
    }
    void visit(const Variable *) override {
        precedence = 1;
    }
    void visit(const Add *) override {
        precedence = 4;
        associative = true;
        left_associative = true;
    }
    void visit(const Sub *) override {
        precedence = 4;
        left_associative = true;
    }
    void visit(const Mul *) override {
        precedence = 3;
        associative = true;
        left_associative = true;
    }
    void visit(const Div *) override {
        precedence = 3;
        left_associative = true;
    }
    void visit(const Frac *) override {
        precedence = 3;
        left_associative = true;
    }
    void visit(const Mod *) override {
        precedence = 3;
    }
    void visit(const Min *) override {
        precedence = 1;
    }
    void visit(const Max *) override {
        precedence = 1;
    }
    void visit(const EQ *) override {
        precedence = 7;
    }
    void visit(const NE *) override {
        precedence = 7;
    }
    void visit(const LT *) override {
        precedence = 6;
    }
    void visit(const LE *) override {
        precedence = 6;
    }
    void visit(const GT *) override {
        precedence = 6;
    }
    void visit(const GE *) override {
        precedence = 6;
    }
    void visit(const And *) override {
        precedence = 11;
        associative = true;
    }
    void visit(const Or *) override {
        precedence = 12;
        associative = true;
    }
    void visit(const Implies *) override {
        precedence = 14;
    }
    void visit(const Not *) override {
        precedence = 2;
    }
    void visit(const Forall *) override {
        precedence = 1;
    }
    void visit(const Exists *) override {
        precedence = 1;
    }
    void visit(const Select *) override {
        precedence = 13;
    }
    void visit(const Load *) override {
        precedence = 1;
    }
    void visit(const Ramp *) override {
        precedence = 1;
    }
    void visit(const Broadcast *) override {
        precedence = 1;
    }
    void visit(const Call *) override {
        precedence = 1;
    }
    void visit(const Let *) override {
        precedence = 1;
    }
};

void IRPrinter::test() {
    Type i32 = Int(32);
    Type f32 = Float(32);
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    ostringstream expr_source;
    expr_source << (x + 3) * (y / 2 + 17);
    internal_assert(expr_source.str() == "((x + 3)*((y/2) + 17))");

    Stmt store = Store::make("buf", (x * 17) / (x - 3), y - 1, Parameter(), const_true(), ModulusRemainder(), Expr());
    Stmt for_loop = For::make("x", -2, y + 2, ForType::Parallel, DeviceAPI::Host, store);
    vector<Expr> args(1);
    args[0] = x % 3;
    Expr call = Call::make(i32, "buf", args, Call::Extern);
    Stmt store2 = Store::make("out", call + 1, x, Parameter(), const_true(), ModulusRemainder(3, 5), Expr());
    Stmt for_loop2 = For::make("x", 0, y, ForType::Vectorized, DeviceAPI::Host, store2);

    Stmt producer = ProducerConsumer::make_produce("buf", for_loop);
    Stmt consumer = ProducerConsumer::make_consume("buf", for_loop2);
    Stmt pipeline = Block::make(producer, consumer);

    Stmt assertion = AssertStmt::make(y >= 3, Call::make(Int(32), "halide_error_param_too_small_i64",
                                                         {string("y"), y, 3}, Call::Extern));
    Stmt block = Block::make(assertion, pipeline);
    Stmt let_stmt = LetStmt::make("y", 17, block);
    Stmt allocate = Allocate::make("buf", f32, MemoryType::Stack, {1023}, const_true(), let_stmt);

    ostringstream source;
    source << allocate;
    std::string correct_source =
        "allocate buf[float32 * 1023] in Stack\n"
        "let y = 17\n"
        "assert(y >= 3, halide_error_param_too_small_i64(\"y\", y, 3))\n"
        "produce buf {\n"
        " parallel (x, -2, y + 2) {\n"
        "  buf[y - 1] = (x*17)/(x - 3)\n"
        " }\n"
        "}\n"
        "consume buf {\n"
        " vectorized (x, 0, y) {\n"
        "  out[x] = buf(x % 3) + 1\n"
        " }\n"
        "}\n";

    if (source.str() != correct_source) {
        internal_error << "Correct output:\n"
                       << correct_source
                       << "Actual output:\n"
                       << source.str();
    }
    std::cout << "IRPrinter test passed\n";
}

ostream &operator<<(ostream &stream, const AssociativePattern &p) {
    stream << "{\n";
    for (size_t i = 0; i < p.ops.size(); ++i) {
        stream << "  op_" << i << " -> " << p.ops[i] << ", id_" << i << " -> " << p.identities[i] << "\n";
    }
    stream << "  is commutative? " << p.is_commutative << "\n";
    stream << "}\n";
    return stream;
}

ostream &operator<<(ostream &stream, const AssociativeOp &op) {
    stream << "Pattern:\n"
           << op.pattern;
    stream << "is associative? " << op.is_associative << "\n";
    for (size_t i = 0; i < op.xs.size(); ++i) {
        stream << "  " << op.xs[i].var << " -> " << op.xs[i].expr << "\n";
        stream << "  " << op.ys[i].var << " -> " << op.ys[i].expr << "\n";
    }
    stream << "\n";
    return stream;
}

ostream &operator<<(ostream &out, const ForType &type) {
    switch (type) {
    case ForType::Serial:
        out << "for";
        break;
    case ForType::Parallel:
        out << "parallel";
        break;
    case ForType::Unrolled:
        out << "unrolled";
        break;
    case ForType::Vectorized:
        out << "vectorized";
        break;
    case ForType::Extern:
        out << "extern";
        break;
    case ForType::GPUBlock:
        out << "gpu_block";
        break;
    case ForType::GPUThread:
        out << "gpu_thread";
        break;
    case ForType::GPULane:
        out << "gpu_lane";
        break;
    }
    return out;
}

ostream &operator<<(ostream &stream, const AnnotationType &type) {
    switch (type) {
    case AnnotationType::Require:
        stream << "requires";
        break;
    case AnnotationType::Ensure:
        stream << "ensures";
        break;
    case AnnotationType::Context:
        stream << "context";
        break;
    case AnnotationType::ContextEverywhere:
        stream << "context_everywhere";
        break;
    case AnnotationType::LoopInvariant:
        stream << "loop_invariant";
        break;
    }
    return stream;
}

ostream &operator<<(ostream &out, const VectorReduce::Operator &op) {
    switch (op) {
    case VectorReduce::Add:
        out << "Add";
        break;
    case VectorReduce::Mul:
        out << "Mul";
        break;
    case VectorReduce::Min:
        out << "Min";
        break;
    case VectorReduce::Max:
        out << "Max";
        break;
    case VectorReduce::And:
        out << "And";
        break;
    case VectorReduce::Or:
        out << "Or";
        break;
    }
    return out;
}

ostream &operator<<(ostream &out, const NameMangling &m) {
    switch (m) {
    case NameMangling::Default:
        out << "default";
        break;
    case NameMangling::C:
        out << "c";
        break;
    case NameMangling::CPlusPlus:
        out << "c++";
        break;
    }
    return out;
}

ostream &operator<<(ostream &stream, const Stmt &ir) {
    if (!ir.defined()) {
        stream << "(undefined)\n";
    } else {
        Internal::IRPrinter p(stream);
        p.print(ir);
    }
    return stream;
}

ostream &operator<<(ostream &stream, const Annotation &ir) {
    if (!ir.defined()) {
        stream << "(undefined)";
    } else {
        Internal::IRPrinter p(stream);
        p.print(ir);
    }
    return stream;
}

ostream &operator<<(ostream &stream, const LoweredFunc &function) {
    stream << function.linkage << " func " << function.name << " (";
    for (size_t i = 0; i < function.args.size(); i++) {
        stream << function.args[i].name;
        if (i + 1 < function.args.size()) {
            stream << ", ";
        }
    }
    stream << ") {\n";
    stream << function.body;
    stream << "}\n\n";
    return stream;
}

void print_def(ostream &stream, const Definition &def, string func_name){
    // First print annotations
    for(const auto &ann: def.annotations()){
        stream << "  " << ann << "\n";
    }

    stream << func_name << "(";
    for (size_t i = 0; i < def.args().size(); i++) {
        stream << def.args()[i];
        if (i + 1 < def.args().size()) {
            stream << ", ";
        }
    }
    stream << ") = ";
    if(def.values().size() == 1){
        stream << def.values()[0];
    } else {
        stream << "(";
        for (size_t i = 0; i < def.values().size(); i++) {
            stream << def.values()[i];
            if (i + 1 < def.values().size()) {
                stream << ", ";
            }
        }
        stream << ")";
    }
    stream << ";\n";
}

ostream &operator<<(ostream &stream, const Function &function) {
    if(function.definition().defined()){
        print_def(stream, function.definition(), function.name());
    }
    for(const auto &update : function.updates()){
        print_def(stream, update, function.name());
    }

    stream << function.name() << " ensures:\n";
    for(const auto &ann: function.func_annotations()){
        stream << "  " << ann << "\n";
    }
    return stream;
}

std::ostream &operator<<(std::ostream &stream, const LinkageType &type) {
    switch (type) {
    case LinkageType::ExternalPlusMetadata:
        stream << "external_plus_metadata";
        break;
    case LinkageType::External:
        stream << "external";
        break;
    case LinkageType::Internal:
        stream << "internal";
        break;
    }
    return stream;
}

std::ostream &operator<<(std::ostream &stream, const Indentation &indentation) {
    for (int i = 0; i < indentation.indent; i++) {
        stream << " ";
    }
    return stream;
}

std::ostream &operator<<(std::ostream &out, const DimType &t) {
    switch (t) {
    case DimType::PureVar:
        out << "PureVar";
        break;
    case DimType::PureRVar:
        out << "PureRVar";
        break;
    case DimType::ImpureRVar:
        out << "ImpureRVar";
        break;
    }
    return out;
}

std::string to_string(const Expr &e){
    if(!e.defined()) return "(undefined)";
    ostringstream out;
    out << e;
    return out.str();
}

std::string to_strings(const std::vector<Expr> &exprs) {
    ostringstream out;
    for (const auto &e : exprs) {
        out << to_string(e) << ", ";
    }
    return out.str();
}

std::string to_string(const Stmt &s){
    if(!s.defined()) return "(undefined)";
    ostringstream out;
    out << s;
    return out.str();
}
std::string to_string(const Annotation &a){
    if(!a.defined()) return "(undefined)";
    ostringstream out;
    out << a;
    return out.str();
}

IRPrinter::IRPrinter(ostream &s, bool semicolon)
    : stream(s), semicolon(semicolon) {
    s.setf(std::ios::fixed, std::ios::floatfield);
}

void IRPrinter::print(const Expr &ir) {
    ScopedValue<bool> old(implicit_parens, false);
    ir.accept(this);
}

void IRPrinter::print_no_parens(const Expr &ir) {
    ScopedValue<bool> old(implicit_parens, true);
    ir.accept(this);
}

void IRPrinter::print_maybe_parens(const Expr &ir, const Expr &outer_expr, bool is_left) {
    Precedence outer;
    outer_expr.accept(&outer);
    Precedence inner;
    ir.accept(&inner);

    bool no_parens = inner.precedence < outer.precedence ||
                     (inner.precedence == outer.precedence &&
                        inner.associative && outer.associative) ||
                     (is_left && inner.precedence == outer.precedence &&
                        inner.left_associative && outer.left_associative);
    ScopedValue<bool> old(implicit_parens, no_parens);
    ir.accept(this);
}

void IRPrinter::print(const Stmt &ir) {
    ir.accept(this);
}

void IRPrinter::print(const Annotation &ir) {
    ir.accept(this);
}

void IRPrinter::print_list(const std::vector<Expr> &exprs) {
    for (size_t i = 0; i < exprs.size(); i++) {
        print_no_parens(exprs[i]);
        if (i < exprs.size() - 1) {
            stream << ", ";
        }
    }
}

void IRPrinter::visit(const IntImm *op) {
    if (op->type == Int(32)) {
        stream << op->value;
    } else {
        stream << "(" << op->type << ")" << op->value;
    }
}

void IRPrinter::visit(const UIntImm *op) {
    stream << "(" << op->type << ")" << op->value;
}

void IRPrinter::visit(const FloatImm *op) {
    switch (op->type.bits()) {
    case 64:
        stream << op->value;
        break;
    case 32:
        stream << op->value << "f";
        break;
    case 16:
        stream << op->value << "h";
        break;
    default:
        internal_error << "Bad bit-width for float: " << op->type << "\n";
    }
}

void IRPrinter::visit(const StringImm *op) {
    stream << "\"";
    for (size_t i = 0; i < op->value.size(); i++) {
        unsigned char c = op->value[i];
        if (c >= ' ' && c <= '~' && c != '\\' && c != '"') {
            stream << c;
        } else {
            stream << "\\";
            switch (c) {
            case '"':
                stream << "\"";
                break;
            case '\\':
                stream << "\\";
                break;
            case '\t':
                stream << "t";
                break;
            case '\r':
                stream << "r";
                break;
            case '\n':
                stream << "n";
                break;
            default:
                string hex_digits = "0123456789ABCDEF";
                stream << "x" << hex_digits[c >> 4] << hex_digits[c & 0xf];
            }
        }
    }
    stream << "\"";
}

void IRPrinter::visit(const ReadPerm *op) {
    stream << "read";
}

void IRPrinter::visit(const Cast *op) {
    stream << op->type << "(";
    print(op->value);
    stream << ")";
}

void IRPrinter::visit(const Variable *op) {
    if (!known_type.contains(op->name) &&
        (op->type != Int(32))) {
        // Handle types already have parens
        if (op->type.is_handle()) {
            stream << op->type;
        } else {
            stream << "(" << op->type << ")";
        }
    }
    stream << op->name;
}

void IRPrinter::open() {
    if (!implicit_parens) {
        stream << "(";
    }
}

void IRPrinter::close() {
    if (!implicit_parens) {
        stream << ")";
    }
}

void IRPrinter::visit(const Add *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " + ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Sub *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " - ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Mul *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << "*";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Div *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << "/";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Frac *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << "\\";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Mod *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " % ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Min *op) {
    stream << "min(";
    print_no_parens(op->a);
    stream << ", ";
    print_no_parens(op->b);
    stream << ")";
}

void IRPrinter::visit(const Max *op) {
    stream << "max(";
    print_no_parens(op->a);
    stream << ", ";
    print_no_parens(op->b);
    stream << ")";
}

void IRPrinter::visit(const EQ *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " == ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const NE *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " != ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const LT *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " < ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const LE *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " <= ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const GT *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " > ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const GE *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " >= ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const And *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " && ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Or *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " || ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Implies *op) {
    open();
    print_maybe_parens(op->a, op, true);
    stream << " ==> ";
    print_maybe_parens(op->b, op, false);
    close();
}

void IRPrinter::visit(const Not *op) {
    stream << "!";
    print(op->a);
}

void IRPrinter::visit(const Forall *op) {
    stream << "(\\forall";
    if(op->type.is_resource()){
        stream << "*";
    }
    for(size_t i= 0; i<op->vars.size(); i++){
        stream << " int " << op->vars[i];
        if (i < op->vars.size() - 1) {
            stream << ", ";
        }
    }
    stream << "; ";

    print_no_parens(op->select);
    stream << "; ";
    print_no_parens(op->main);
    stream << ")";
}

void IRPrinter::visit(const Exists *op) {
    stream << "(\\exists";
    for(size_t i= 0; i<op->vars.size(); i++){
        stream << " int " << op->vars[i];
        if (i < op->vars.size() - 1) {
            stream << ", ";
        }
    }

    print_no_parens(op->select);
    stream << "; ";
    print_no_parens(op->main);
    stream << ")";
}

void IRPrinter::visit(const Select *op) {
    stream << "select(";
    print_no_parens(op->condition);
    stream << ", ";
    print_no_parens(op->true_value);
    stream << ", ";
    print_no_parens(op->false_value);
    stream << ")";
}

void IRPrinter::visit(const Load *op) {
    const bool has_pred = !is_const_one(op->predicate);
    const bool show_alignment = op->type.is_vector() && op->alignment.modulus > 1;
    if (has_pred) {
        open();
    }
    if (!known_type.contains(op->name)) {
        stream << "(" << op->type << ")";
    }
    stream << op->name << "[";
    print_no_parens(op->index);
    if (show_alignment) {
        stream << " aligned(" << op->alignment.modulus << ", " << op->alignment.remainder << ")";
    }
    stream << "]";
    if (has_pred) {
        stream << " if ";
        print(op->predicate);
        close();
    }
    if(op->lemma.defined()){
        stream << " /*lemma: ";
        print(op->lemma);
        stream << "*/ ";
    }
    
}

void IRPrinter::visit(const Ramp *op) {
    stream << "ramp(";
    print_no_parens(op->base);
    stream << ", ";
    print_no_parens(op->stride);
    stream << ", " << op->lanes << ")";
}

void IRPrinter::visit(const Broadcast *op) {
    stream << "x" << op->lanes << "(";
    print_no_parens(op->value);
    stream << ")";
}

void IRPrinter::visit(const Call *op) {
    // TODO: Print indication of C vs C++?
    if (!known_type.contains(op->name) &&
        (op->type != Int(32))) {
        if (op->type.is_handle()) {
            stream << op->type;  // Already has parens
        } else {
            stream << "(" << op->type << ")";
        }
    }
    stream << op->name << "(";
    print_list(op->args);
    stream << ")";
    if(op->func.defined() && Function((op->func)).outputs() > 1){
        stream << "[" << op->value_index << "]";
    }
}

void IRPrinter::visit(const Let *op) {
    ScopedBinding<> bind(known_type, op->name);
    open();
    stream << "let " << op->name << " = ";
    print(op->value);
    stream << " in ";
    print(op->body);
    close();
}

void IRPrinter::visit(const LetStmt *op) {
    ScopedBinding<> bind(known_type, op->name);
    stream << get_indent() << "let " << op->name << " = ";
    print_no_parens(op->value);
    stream << "\n";

    print(op->body);
}

void IRPrinter::visit(const AssertStmt *op) {
    stream << get_indent() << "assert(";
    print_no_parens(op->condition);
    stream << ", ";
    print_no_parens(op->message);
    stream << ")" << (semicolon ? ";\n" : "\n");
}

void IRPrinter::visit(const ProducerConsumer *op) {
    stream << get_indent();
    if (op->is_producer) {
        stream << "produce " << op->name << " {\n";
    } else {
        stream << "consume " << op->name << " {\n";
    }
    indent++;
    print(op->body);
    indent--;
    stream << get_indent() << "}\n";
}

void IRPrinter::visit(const For *op) {
    ScopedBinding<> bind(known_type, op->name);
    indent++;
    for(const Annotation &a : op->annotations){
        stream << get_indent();
        print(a);
        stream << "\n";
    }
    indent--;
    stream << get_indent() << op->for_type << op->device_api << " (" << op->name << ", ";
    print_no_parens(op->min);
    stream << ", ";
    print_no_parens(op->extent);
    stream << ") {\n";

    indent++;
    print(op->body);
    indent--;

    stream << get_indent() << "}\n";
}

void IRPrinter::visit(const Acquire *op) {
    stream << get_indent() << "acquire (";
    print_no_parens(op->semaphore);
    stream << ", ";
    print_no_parens(op->count);
    stream << ") {\n";
    indent++;
    print(op->body);
    indent--;
    stream << get_indent() << "}\n";
}

void IRPrinter::print_lets(const Let *let) {
    stream << get_indent();
    ScopedBinding<> bind(known_type, let->name);
    stream << "let " << let->name << " = ";
    print_no_parens(let->value);
    stream << " in\n";
    if (const Let *next = let->body.as<Let>()) {
        print_lets(next);
    } else {
        stream << get_indent();
        print_no_parens(let->body);
        stream << "\n";
    }
}

void IRPrinter::visit(const Store *op) {
    stream << get_indent();
    const bool has_pred = !is_const_one(op->predicate);
    const bool show_alignment = op->value.type().is_vector() && (op->alignment.modulus > 1);
    if (has_pred) {
        stream << "predicate (";
        print_no_parens(op->predicate);
        stream << ")\n";
        indent++;
        stream << get_indent();
    }
    stream << op->name << "[";
    print_no_parens(op->index);
    if (show_alignment) {
        stream << " aligned("
               << op->alignment.modulus
               << ", "
               << op->alignment.remainder << ")";
    }
    stream << "] = ";
    if (const Let *let = op->value.as<Let>()) {
        // Use some nicer line breaks for containing Lets
        stream << "\n";
        indent += 2;
        print_lets(let);
        indent -= 2;
    } else {
        // Just print the value in-line
        print_no_parens(op->value);
    }
    stream << (semicolon ? ";\n" : "\n");
    if (has_pred) {
        indent--;
    }
    if(op->lemma.defined() && !is_const_zero(op->lemma)){
        stream << "/* lemma: ";
        print(op->lemma);
        stream << "*/";
    }
}

void IRPrinter::visit(const Provide *op) {
    stream << get_indent() << op->name << "(";
    print_list(op->args);
    stream << ") = ";
    if (op->values.size() > 1) {
        stream << "{";
    }
    print_list(op->values);
    if (op->values.size() > 1) {
        stream << "}";
    }

    stream << (semicolon ? ";\n" : "\n");
}

void IRPrinter::visit(const Allocate *op) {
    ScopedBinding<> bind(known_type, op->name);
    stream << get_indent() << "allocate " << op->name << "[" << op->type;
    for (size_t i = 0; i < op->extents.size(); i++) {
        stream << " * ";
        print(op->extents[i]);
    }
    stream << "]";
    if (op->memory_type != MemoryType::Auto) {
        stream << " in " << op->memory_type;
    }
    if (!is_const_one(op->condition)) {
        stream << " if ";
        print(op->condition);
    }
    if (op->new_expr.defined()) {
        stream << "\n";
        stream << get_indent() << " custom_new { ";
        print_no_parens(op->new_expr);
        stream << " }";
    }
    if (!op->free_function.empty()) {
        stream << "\n";
        stream << get_indent() << " custom_delete { " << op->free_function << "(" << op->name << "); }";
    }
    stream << (semicolon ? ";\n" : "\n");
    print(op->body);
}

void IRPrinter::visit(const Free *op) {
    stream << get_indent() << "free " << op->name;
    stream << (semicolon ? ";\n" : "\n");
}

void IRPrinter::visit(const Realize *op) {
    ScopedBinding<> bind(known_type, op->name);
    stream << get_indent() << "realize " << op->name << "(";
    for (size_t i = 0; i < op->bounds.size(); i++) {
        stream << "[";
        print_no_parens(op->bounds[i].min);
        stream << ", ";
        print_no_parens(op->bounds[i].extent);
        stream << "]";
        if (i < op->bounds.size() - 1) {
            stream << ", ";
        }
    }
    stream << ")";
    if (op->memory_type != MemoryType::Auto) {
        stream << " in " << op->memory_type;
    }
    if (!is_const_one(op->condition)) {
        stream << " if ";
        print(op->condition);
    }
    stream << " {\n";

    indent++;
    print(op->body);
    indent--;

    stream << get_indent() << "}\n";
}

void IRPrinter::visit(const Prefetch *op) {
    stream << get_indent();
    const bool has_cond = !is_const_one(op->condition);
    if (has_cond) {
        stream << "if (";
        print_no_parens(op->condition);
        stream << ") {\n";
        indent++;
        stream << get_indent();
    }
    stream << "prefetch " << op->name << "(";
    for (size_t i = 0; i < op->bounds.size(); i++) {
        stream << "[";
        print_no_parens(op->bounds[i].min);
        stream << ", ";
        print_no_parens(op->bounds[i].extent);
        stream << "]";
        if (i < op->bounds.size() - 1) {
            stream << ", ";
        }
    }
    stream << ")\n";
    if (has_cond) {
        indent--;
        stream << get_indent() << "}\n";
    }
    print(op->body);
}

void IRPrinter::visit(const Block *op) {
    print(op->first);
    print(op->rest);
}

void IRPrinter::visit(const Fork *op) {
    vector<Stmt> stmts;
    stmts.push_back(op->first);
    Stmt rest = op->rest;
    while (const Fork *f = rest.as<Fork>()) {
        stmts.push_back(f->first);
        rest = f->rest;
    }
    stmts.push_back(rest);

    stream << get_indent() << "fork ";
    for (const Stmt &s : stmts) {
        stream << "{\n";
        indent++;
        print(s);
        indent--;
        stream << get_indent() << "} ";
    }
    stream << "\n";
}

void IRPrinter::visit(const IfThenElse *op) {
    stream << get_indent();
    while (true) {
        stream << "if (";
        print_no_parens(op->condition);
        stream << ") {\n";
        indent++;
        print(op->then_case);
        indent--;

        if (!op->else_case.defined()) {
            break;
        }

        if (const IfThenElse *nested_if = op->else_case.as<IfThenElse>()) {
            stream << get_indent() << "} else ";
            op = nested_if;
        } else {
            stream << get_indent() << "} else {\n";
            indent++;
            print(op->else_case);
            indent--;
            break;
        }
    }

    stream << get_indent() << "}\n";
}

void IRPrinter::visit(const Evaluate *op) {
    indent++;
    for(const Annotation &a : op->annotations){
        stream << get_indent();
        print(a);
        stream << "\n";
    }
    indent--;
    stream << get_indent();
    print_no_parens(op->value);
    stream << ((semicolon ? ";\n" : "\n"));
}

void IRPrinter::visit(const Shuffle *op) {
    if (op->is_concat()) {
        stream << "concat_vectors(";
        print_list(op->vectors);
        stream << ")";
    } else if (op->is_interleave()) {
        stream << "interleave_vectors(";
        print_list(op->vectors);
        stream << ")";
    } else if (op->is_extract_element()) {
        stream << "extract_element(";
        print_list(op->vectors);
        stream << ", " << op->indices[0] << ")";
    } else if (op->is_slice()) {
        stream << "slice_vectors(";
        print_list(op->vectors);
        stream << ", " << op->slice_begin()
               << ", " << op->slice_stride()
               << ", " << op->indices.size()
               << ")";
    } else if (op->is_broadcast()) {
        stream << "broadcast(";
        print_list(op->vectors);
        stream << ", " << op->broadcast_factor() << ")";
    } else {
        stream << "shuffle(";
        print_list(op->vectors);
        stream << ", ";
        for (size_t i = 0; i < op->indices.size(); i++) {
            print_no_parens(op->indices[i]);
            if (i < op->indices.size() - 1) {
                stream << ", ";
            }
        }
        stream << ")";
    }
}

void IRPrinter::visit(const VectorReduce *op) {
    stream << "("
           << op->type
           << ")vector_reduce("
           << op->op
           << ", "
           << op->value
           << ")";
}

void IRPrinter::visit(const Atomic *op) {
    if (op->mutex_name.empty()) {
        stream << get_indent() << "atomic {\n";
    } else {
        stream << get_indent() << "atomic (";
        stream << op->mutex_name;
        stream << ") {\n";
    }
    indent += 2;
    print(op->body);
    indent -= 2;
    stream << get_indent() << "}\n";
}

void IRPrinter::visit(const Ghost *op) {
    stream << get_indent() << "ghost\n";
    indent++;
    print(op->ghost);
    indent--;
}

void IRPrinter::visit(const AnnExpr *op) {
    stream << op->ann_type << " ";
    print_no_parens(op->condition);
}


void IRPrinter::visit(const Permission *op) {
    stream << op->ann_type << " ";
    if(!op->forall_vars.empty()){
        stream << "(\\forall*";
        for(auto & var: op->forall_vars)
            stream << " int " << var;
        stream << "; ";

        print_no_parens(op->antecedent);
        stream << "; ";
    } else {
        //Check if the right hand side is simply true
        if( !is_const_true(op->antecedent) ){
            print_no_parens(op->antecedent);
            stream << " ==> ";
        }
    }
    
    stream << "Perm(";
    print_no_parens(op->variable);
    stream << ", ";
    print_no_parens(op->permission);
    stream << ")";

    if(!op->forall_vars.empty()) stream << ")";
}

}  // namespace Internal
}  // namespace Halide
