#include "IRMutator.h"
#include "IROperator.h"
#include "IRVisitor.h"
#include "Simplify.h"
#include "Var.h"
#include "VectorizeLoops.h"
#include "PVLPrinter.h"

#include "CodeGen_C.h"

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::tuple;
using std::set;
using std::string;
using std::vector;

namespace {

Expr add(const Expr &original, const Expr &added){
    Expr result;
    if(original.defined()){
        result = original && added;
    } else {
        result = added;
    }
    return result;
}

struct BufferInfo {
    string name;
    Expr bound;
    Expr index;
    vector<Expr> pred_args;
    vector<string> forall_vars;
    vector<Expr> implicit_args;
    Type type;
};

// The constraints of tupled output functions are all put in par0
void get_buffer_annotations(const Parameter &par0, const Parameter &buf, vector<Annotation> &res){
    Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), buf.name() + ".buffer");
    int dim = buf.dimensions();
    for(int i=0; i<dim; i++){
        Expr constraint;
        if(par0.min_constraint(i).defined()){
            Expr min_val = Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
            Expr new_constraint = min_val == par0.min_constraint(i);
            constraint = add(constraint, new_constraint);
        }
            
        if(par0.extent_constraint(i).defined()){
            Expr extent_val = Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
            Expr new_constraint = extent_val == par0.extent_constraint(i);
            constraint = add(constraint, new_constraint);
        }

        if(par0.stride_constraint(i).defined()){
            Expr stride_val = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
            Expr new_constraint = stride_val == par0.stride_constraint(i);
            constraint = add(constraint, new_constraint);
        }
        if(constraint.defined()){
            res.emplace_back(AnnExpr::make(AnnotationType::Context, constraint));
        }
    }
}

class UpdateBufferAnnotations: public IRMutator {
    using IRMutator::visit;

    const map<string, vector<tuple<Expr,Expr,Expr>>> &buffer_constraints;

    enum VarType {
        Min,
        Extent,
        Stride
    };

    Expr get_min(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() ) 
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<0>(constraints->second[i]);
        if(e.defined()) return e;
        
        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_min, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".min." + std::to_string(i));
        }
    }

    Expr get_extent(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() )
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<1>(constraints->second[i]);
        if(e.defined()) return e;

        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_extent, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".extent." + std::to_string(i));
        }
    }

     Expr get_stride(string name, int i){
        auto constraints = buffer_constraints.find(name);
        user_assert(constraints != buffer_constraints.end() ) 
            << "Pipeline annotation for images contain a call to a non-image type, which is not allowed: " << name;

        Expr e = std::get<2>(constraints->second[i]);
        if(e.defined()) return e;
        
        if(top_level){
            Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), name + ".buffer");
            return Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
        } else {
            return Variable::make(Int(32), name + ".stride." + std::to_string(i));
        }
    }

    Expr visit(const Variable *op) override {
        if(!top_level)
            return op;
            
        string name = op->name;
        int num = -1;
        VarType prop;

        if(ends_with(name, ".0")){
            num = 0;
        } else if(ends_with(name, ".1")) {
            num = 1;
        } else if(ends_with(name, ".2")) {
            num = 2;
        } else if(ends_with(name, ".3")) {
            num = 3;
        } else if(ends_with(name, ".4")) {
            num = 4;
        } else if(ends_with(name, ".5")) {
            num = 5;
        } else if(ends_with(name, ".6")) {
            num = 6;
        } else if(ends_with(name, ".7")) {
            num = 7;
        } else if(ends_with(name, ".8")) {
            num = 8;
        } else if(ends_with(name, ".9")) {
            num = 9;
        }

        
        
        if(num >= 0){
            name.erase(name.length()-2);
            if(ends_with(name, ".min")){
                prop = VarType::Min;
                name.erase(name.length()-4);
            } else if(ends_with(name, ".extent")){
                prop = VarType::Extent;
                name.erase(name.length()-7);
            } else if(ends_with(name, ".stride")){
                prop = VarType::Stride;
                name.erase(name.length()-7);
            } else {
                return op;
            }

            if(name != ""){
                switch(prop) {
                    case VarType::Min:
                        return get_min(name, num);
                    case VarType::Extent:
                        return get_extent(name, num);
                    case VarType::Stride:
                        return get_stride(name, num);
                }
            }
        }

        return op;
    }

    Expr visit(const Call *call) override {
        if(call->is_extern() || call->is_intrinsic()){
            return IRMutator::visit(call);
        }
        
        string name = call->name;
        Type type;
        size_t dimensions;
        
        if(call->call_type == Call::CallType::Halide){
            // Ugly, but calls to images get inlined to some sort of function ending on _im, 
            // which are removed elsewhere in the pipeline, but not here yet
            
            // First remove the unique modifiers to name
            size_t pos = name.find('$');
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }
            if(ends_with(name, "_im"))
                name.erase(name.length()-3);

            Function f(call->func);
            user_assert(f.outputs() == 1)
                << "Function " << name << "has zero or more than one outputs, which we do not yet support";
            type = f.output_types().front();
            dimensions = f.dimensions();
        } else {
            user_assert(call->call_type == Call::CallType::Image) 
                << "Annotations for images contain a call to a non-image type, which is not allowed: \"" << call->name << "\"";
            type = call->param.type();
            dimensions = call->param.dimensions();
        }

        Expr index;
        // Mutate the args
        user_assert(dimensions == call->args.size()) 
            << "Annotations for images contain a call to an image with "<< call->args.size() 
            << " arguments, but the dimensionality of: \"" << call->name << "\" is " << dimensions;
        
        vector<Expr> new_args(dimensions);
        for (size_t i = 0; i < dimensions; i++) {
            const Expr &old_arg = call->args[i];
            Expr new_arg = mutate(old_arg);
            new_args[i] = std::move(new_arg);
        }
        
        for(size_t i=0; i < dimensions;i++){
            Expr min = get_min(name, i);
            Expr stride = get_stride(name, i);
            Expr added_dimension = (new_args[i] - min) * stride;
            if(i==0)
                index = added_dimension;
            else
                index = Add::make(index, added_dimension);
        }
        if(dimensions==0){
            index = 0;
        }

        index = mutate(index);

        if(top_level){
            if(is_output){
                string top_level_name = call->name;
                if(ends_with(top_level_name, "_im"))
                    top_level_name.erase(top_level_name.length()-3);
                name = top_level_name + ".buffer.host";
            } else {
                name = name + ".buffer.host";
            }
        }
        return Load::make(type, name, index, Buffer<>(), call->param, const_true(), ModulusRemainder(), Expr());
    }

public:
    UpdateBufferAnnotations(const map<string, vector<tuple<Expr,Expr,Expr>>> &buffer_constraints, bool top_level) 
      : buffer_constraints(buffer_constraints), top_level(top_level), is_output(false) {}

    bool top_level;
    bool is_output;
};

class AnnotationMaker {
    Expr condition;

    bool is_perm = false;

    vector<string> forall_vars;
    Expr bound;
    Expr load;
public:
    AnnotationMaker(BufferInfo &buffer_info) {
        is_perm = true;
        load = Load::make(buffer_info.type, buffer_info.name, 
            buffer_info.index, Buffer<>(), Parameter(), const_true(), ModulusRemainder(), Expr());
        bound = buffer_info.bound;
        forall_vars = buffer_info.forall_vars;
    }

    AnnotationMaker(BufferInfo &buffer_info, Expr &condition) : AnnotationMaker(buffer_info) {
        this->condition = condition;
        is_perm = false;
     }


    Annotation create_annotation(bool is_parallel, Expr &readfactor) const {
        AnnotationType anntype = is_parallel ? AnnotationType::Context : AnnotationType::LoopInvariant;
        if(is_perm){
            Expr perm = forall(forall_vars, bound, Perm(load, read(readfactor)));
            return AnnExpr::make(anntype, perm);
        } else {
            return AnnExpr::make(anntype, condition);
        }
    }

    void update(UpdateBufferAnnotations &uba) {
        condition = simplify_ann(uba.mutate(condition));
        bound = simplify_ann(uba.mutate(bound));
        load = simplify_ann(uba.mutate(load));
    }
};

vector<Annotation> create_annotations(vector<AnnotationMaker> anns, bool is_parallel, Expr &readfactor){
    vector<Annotation> res;
    for(auto const &a: anns){
        res.emplace_back(a.create_annotation(is_parallel, readfactor));
    }
    return res;
}

class ContainsFunctionCall: public IRVisitor {
public:
    ContainsFunctionCall(string function) : contains_call(false), function(function) { }

    bool contains_call;
private:
    string function;
    
    using IRVisitor::visit;

    void visit(const Load *load) override {
        IRVisitor::visit(load);

        if(load->name == function){
            contains_call = true;
        }
    }

    // Evaluate is to annotate GPU barriers
    void visit(const Evaluate *op) override {
        op->value.accept(this);
        // Gpu barriers may say something about permissions
        for(const Annotation &a : op->annotations){
            a.accept(this);
        }
    }

    void visit(const For *op) override {
        op->min.accept(this);
        op->extent.accept(this);
        op->body.accept(this);
        // We should visit annotations now, since if they refer to a function, the annotation information is needed.
        for(const Annotation &a : op->annotations){
            a.accept(this);
        }
    }
};

class AddParameterAnnotations : public IRMutator {
    map<string,vector<AnnotationMaker>> proven_annotations;
    map<string, vector<tuple<Expr,Expr,Expr>>> buffer_constraints;

    // Input buffers are used as constants, thus no permissions necessary
    bool const_input_buffers = false;

    vector<Expr> parallel_read_factor;

    using IRMutator::visit;

    Stmt visit(const For *for_loop) override {
        if(for_loop->is_parallel()){
            parallel_read_factor.push_back(for_loop->extent * 2);
        } else {
            parallel_read_factor.push_back(make_const(Int(32), 2));
        }
        Expr currentFactor = make_const(Int(32), 2);
        for(auto f : parallel_read_factor){
            currentFactor = currentFactor * f;
        }

        vector<Annotation> loop_annotations;
        for(auto &it: proven_annotations){
            string name = it.first;
            ContainsFunctionCall cfc(name);
            for_loop->accept(&cfc);
            if(cfc.contains_call){
                vector<Annotation> new_annotations = create_annotations(it.second, for_loop->is_parallel(), currentFactor);
                loop_annotations.insert(loop_annotations.end(), new_annotations.begin(), new_annotations.end());
            }
        }

        loop_annotations.insert(loop_annotations.end(), for_loop->annotations.begin(), for_loop->annotations.end() );

        Stmt body = mutate(for_loop->body);

        parallel_read_factor.pop_back();

        return For::make(for_loop->name,
                             for_loop->min,
                             for_loop->extent,
                             for_loop->for_type,
                             for_loop->device_api,
                             body,
                             loop_annotations);
    }

    BufferInfo process_dimensions(const Parameter &par){
        vector<Expr> pred_args;
        vector<string> forall_vars;
        vector<Expr> implicit_args;
        Expr bound;
        vector<tuple<Expr,Expr,Expr>> buffer_dims;
        Expr index;
        Expr buffer = Variable::make(type_of<struct halide_buffer_t *>(), par.name() + ".buffer");
        // Expr buffer_in = Variable::make(par.type(), par.name());
        Expr host = Call::make(Handle(), Call::buffer_get_host, {buffer}, Call::Extern);
        if(par.dimensions() == 0){
            index = 0;
        }
        for (int i = 0; i < par.dimensions(); i++) {
            Expr min = par.min_constraint(i);
            Expr extent = par.extent_constraint(i);
            Expr stride = par.stride_constraint(i);
            buffer_dims.emplace_back(min, extent, stride);
            if(!min.defined()){
                internal_error << "Buffers need to have constraints for HaliVer to work: No min for " << par.name() << ", dimension: " << i;
                min = Variable::make(Int(32), par.name() + ".min." + std::to_string(i));
            }
            if(!extent.defined()){
                internal_error << "Buffers need to have constraints for HaliVer to work: No extent for " << par.name() << ", dimension: " << i;
                extent = Variable::make(Int(32), par.name() + ".extent." + std::to_string(i));
            }
            if(!stride.defined()){
                internal_error << "Buffers need to have stride constraints for HaliVer to work: No stride for " << par.name() << ", dimension: " << i;
                stride = Call::make(Int(32), Call::buffer_get_stride, {buffer, i}, Call::Extern);
                stride = Variable::make(Int(32), par.name() + ".stride." + std::to_string(i));
            }
            
            Var var = Var::implicit(i);
            pred_args.emplace_back(var);
            pred_args.emplace_back(min);
            pred_args.emplace_back(extent);
            forall_vars.emplace_back(var.name());
            implicit_args.emplace_back(Var(var.name()));
            Expr new_bound = min <= var && var < min + extent;
            Expr new_index = (var - min) * stride;
            if(i==0){
                bound = new_bound;
                index = new_index;
            } else {
                bound = bound && new_bound;
                index = index + new_index;
            }
        }

        string name = par.name();
        size_t pos = name.find('$');
        if (pos != std::string::npos) {
            name = name.substr(0, pos);
        }

        buffer_constraints[name] = buffer_dims;

        BufferInfo result;
        result.name = par.name();
        result.bound = bound;
        result.index = simplify_ann(index);
        result.pred_args = pred_args;
        result.forall_vars = forall_vars;
        result.implicit_args = implicit_args;
        result.type = par.type();

        return result;
    }

    void get_output_annotations(const Function &f){
        user_assert(f.output_buffers().size()>0) << "Need at least dimension one for output buffer";
        Parameter dim_0 = f.output_buffers()[0];
        BufferInfo info = process_dimensions(dim_0);

        for(int idx=0; idx<f.outputs(); idx++){
            Parameter par = f.output_buffers()[idx];
            get_buffer_annotations(dim_0, par, top_level);
            
            Expr load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(), par,const_true(), ModulusRemainder(), Expr());
            Expr perm_top_level;
            if(dim_0.dimensions() == 0){
                perm_top_level = Perm(load, write() );
            } else {
                perm_top_level = forall(info.forall_vars, info.bound, Perm(load, write() ));
            }
            top_level.emplace_back(AnnExpr::make(AnnotationType::Context, perm_top_level));

            for(const auto& ann :par.annotations()){
                const auto *ann_expr = ann.as<AnnExpr>();
                user_assert(ann_expr) << "No permission annotations allowed";
                user_assert(ann_expr->ann_type == AnnotationType::Ensure) << "Only ensure annotations allowed concerning top level";
                Expr cond = ann_expr->condition;
                cond = simplify_ann(uba->mutate(cond));
                if(dim_0.dimensions() > 0){
                    cond = forall(info.forall_vars, info.bound, cond);
                }

                top_level.emplace_back(AnnExpr::make(AnnotationType::Ensure, cond));
            }
        }
    }

    void get_input_annotations(const Parameter &par){
        if(!par.is_buffer())
            return;
    
        get_buffer_annotations(par, par, top_level);
        BufferInfo info = process_dimensions(par);

        
        // We model input buffers with pure functions, since this has less costs for checking annotations.
        Expr pure_call = Call::make(par.type(), "pure_" + par.name(), {info.index}, Call::Extern);

        // When non constant, we need to add this info together with read permission to each loop
        if(!const_input_buffers){
            AnnotationMaker pmaker(info);
            proven_annotations[par.name()].emplace_back(pmaker);

            Expr load_internal = Load::make(info.type, info.name, 
            info.index, Buffer<>(), Parameter(), const_true(), ModulusRemainder(), Expr());
            // This adds f(x,y) == pure_f(x,y)
            Expr eq = Forall::make(info.forall_vars, info.bound, trigger(load_internal) == pure_call);
            AnnotationMaker is_pure(info, eq);
            proven_annotations[par.name()].emplace_back(is_pure);
        }       
        
        // We need to add the same top level
        Expr load = Load::make(par.type(), par.name()+".buffer.host", info.index, Buffer<>(),
            par,const_true(), ModulusRemainder(), Expr());
            
        if(!const_input_buffers){
            // This adds read permission
            Expr perm_top_level = forall(info.forall_vars, info.bound, Perm(load, read(2)) );
            top_level.emplace_back(AnnExpr::make(AnnotationType::Context, perm_top_level));
        }
        top_level.emplace_back(AnnExpr::make(AnnotationType::Context, 
            Forall::make(info.forall_vars, info.bound, trigger(load) == pure_call)));

        for(const auto& ann :par.annotations()){
            const auto *ann_expr = ann.as<AnnExpr>();
            user_assert(ann_expr) << "No permission annotations allowed";

            AnnotationType annt = ann_expr->ann_type == AnnotationType::ContextEverywhere ? AnnotationType::Context : ann_expr->ann_type;
            user_assert(ann_expr->ann_type == AnnotationType::Require || ann_expr->ann_type == AnnotationType::Context)
                << "Annotation type should be require or context.";
            Expr cond = add_trigger(ann_expr->condition, par.name(), info.implicit_args, info.implicit_args, true);
            if(!const_input_buffers){
                // Const input buffers stay.. constant, VerCors can infer this, so no need to repeat
                Expr condition = simplify(cond);
                condition = forall(info.forall_vars, info.bound, condition);
                proven_annotations[par.name()].emplace_back(AnnotationMaker(info, condition));
            }
            top_level.emplace_back(AnnExpr::make(annt, forall(info.forall_vars, info.bound, simplify_ann(uba->mutate(cond)))));
        }
    }

public:
    vector<Annotation> top_level;
    UpdateBufferAnnotations* uba;

    AddParameterAnnotations(vector<Parameter> input, vector<Function> output
        , vector<Annotation> pipeline_annotations, bool const_input_buffers): const_input_buffers(const_input_buffers) {
        uba = new UpdateBufferAnnotations(buffer_constraints, true);

        for(auto &i: input){
            get_input_annotations(i);
        }

        uba->is_output = true;

        for(auto &o: output){
            get_output_annotations(o);
        }

        for(auto &p: pipeline_annotations){
            top_level.emplace_back(simplify(uba->mutate(p)));
        }

        uba->top_level = false;


        for(auto &it: proven_annotations){
            for(size_t i=0; i<it.second.size(); i++){
                it.second[i].update(*uba);
            }
        }
    }
};

class UpdateInputBufferCallsToFunction: public IRMutator {
    set<string> input;
    bool in_annotation;

    using IRMutator::visit;

    Annotation visit(const AnnExpr *ann) override {
        in_annotation = true;
        Annotation result = IRMutator::visit(ann);
        in_annotation = false;
        return result;
    }

    Expr visit(const Load *op) override {
        if(input.find(op->name) != input.end() && in_annotation){
            internal_assert(!op->predicate.defined() || is_const_true(op->predicate)) << "Cannot have a predicate here.";
            Expr index = mutate(op->index);
            return Call::make(op->type, "pure_" + op->name, {index}, Call::Extern);
        }

        return IRMutator::visit(op);
    }

public:
    UpdateInputBufferCallsToFunction(vector<Parameter> inp) : in_annotation(false) {
        for(auto &i: inp){
            if(i.is_buffer()){
                this->input.emplace(i.name());
            }
        }
    }
};

}  // namespace

pair<Stmt, vector<Annotation>> add_pipeline_annotations(const Stmt &stmt, vector<Parameter> input,
     vector<Function> output, vector<Annotation> pipeline_anns, bool const_input_buffers) {
    Stmt s = stmt;
    // if(!const_input_buffers){
        UpdateInputBufferCallsToFunction uibctf(input);
        s = uibctf.mutate(s);
    // }

    AddParameterAnnotations apa(input, output, pipeline_anns, const_input_buffers);
    s = apa.mutate(s);
    return pair<Stmt, vector<Annotation>>(s, apa.top_level);
}

}  // namespace Internal
}  // namespace Halide
