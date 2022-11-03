#include "Halide.h"
#include <stdio.h>

using namespace Halide;

int main(int argc, char *argv[]) {
    bool old = false;
    
    int option = 3;

    Func in("in"), a("a"), b("b"), c("c"), f("f"), g("g"), out("out");
    Var x("x"), y("y");
    ImageParam inp(type_of<int>(), 1, "inp");


    if(old){
        // Input
        a(x) = 4*x + 1; 
        b(x) = 100 - 2*x - 1;
    
        // Output
        c(x) = a(x) + b(x);
        c.ensure(c(x) == a(x) + b(x));
    
        // Output for options 3 & 4
        out(x) = c(x-1) + c(x) + c(x+1);
        out.ensure(out(x) == c(x-1) + c(x) + c(x+1));
    
        Var bx("bx"), tx("tx");
    
        if(option == 1){
            c.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf); 
        } else if(option == 2){
    
            c.split(x, bx, tx, 32*2, TailStrategy::GuardWithIf);
            c.unroll(tx, 2);
            c.gpu(bx, tx);
        } else if(option == 3){
            out.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
    
            c.compute_root();
            c.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
        } else if(option ==4) {
            out.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
            c.compute_at(out,bx)
                .split(x, bx, tx, 32, TailStrategy::GuardWithIf)
                .gpu_threads(tx);
    
            // Other scheduling options: vectorize, tiling, unrolling
            // Other algorithm options: reductions (summing vallues)
        }
        
    
    
    
        a.compute_root();
        b.compute_root(); 
        if(option == 1 || option == 2){
            c.bound(x,0,128);
            c.output_buffer().dim(0).set_min(0);
        } else if(option == 3 || option == 4){
            out.bound(x,1,128);
            out.output_buffer().dim(0).set_min(0);  
        }
    } else {
        in(x) = 2 * x;
        in.ensure(in(x) % 2 == 0);
        in.compute_root();

        f(x,y) = in(x) + in(y);
        f.ensure(f(x,y) == in(x) + in(y));
        f(x,0) = f(x,0) + f(x,1);
        f.ensure(f(x,y) % 2 == 0);
        g(x,y) = f(x,y)+ f(x+1,y) + f(x+2,y);
        g.ensure(g(x,y) % 2 == 0);


        g.compute_root();
        out(x,y) = g(x,y);

        out.bound(x,0,4);
        out.bound(y,0,3);
        out.output_buffer().dim(0).set_min(0);
        out.output_buffer().dim(1).set_min(0);

        f.compute_at(g,y);
    }
    

    bool add_runtime = false;
    bool aot = false;
    std::string s_runntime ("--runtime");
    std::string s_aot ("--aot");

    if(argc > 1)
        for(int i=1; i<argc; i++){
            if(s_runntime.compare(argv[i]) == 0){
                add_runtime = true;
                aot = false;
            }
            if(s_aot.compare(argv[i]) == 0){
                add_runtime = false;
                aot = true;
            }
        }

    Target target = get_host_target();
    Target new_target = target
      .with_feature(Target::OpenCL)
      .with_feature(Target::NoAsserts)
      .with_feature(Target::NoBoundsQuery)
      ;
    assert(host_supports_target_device(new_target));

    Func res;
    if(option == 1 || option == 2)
        res = c;
    else if(option == 3 || option == 4)
        res = out;
    
    std::vector<Halide::Argument> inputs;
    if(old)
        inputs = {};
    else
        inputs = {};

    if(add_runtime){
        compile_standalone_runtime("opencl_runtime.o",new_target);
        res.compile_to_header("even_c.h", inputs, "even", new_target);
    }

    if(!add_runtime && !aot){
        res.compile_to_c("even_c.cpp", inputs, "even", new_target);
        res.compile_to_pvl("even_pvl.pvl", inputs, "even", new_target);
    }

    if(aot){
        res.compile_to_static_library("even_static", inputs, "even", new_target);
        res.print_loop_nest();
    }

    




    // f(x,y) = 2*(x+y)+1;
    // f.ensure(f(x,y) % 2 == 1);
    // f(x, x+1) = f(x,x) * 3 + f(x, 1);
    // f.ensure(f(x,y) % 2 == 0);

    // g(x) = f(x, x+1);

    // k(x,y) = 0;
    // k.ensure(k(x,y) == 0);
    // k(x, r2.x - r2.y) = k(x, r2.x) + k(x,0) + r2.y;

    // h(x) = k(x,x);
    // h.ensure(h(x) == 0);
    // h(x) += g(x*8 + r -4) + 1 ;
    // h.loop_invariant(h(x) % 2 == r % 2);
    // h.ensure(h(x) % 2 == 0);

    // in(x) = f(x,x) + h(x+1);//2*x*x+1;
    // //in.context_perm(in(x), frac(1,1));
    // in.ensure(in(x) % 2 == 1);
    // in(x) = select(x < 100, in(x) + 1, 0);
    // in.ensure(in(x) % 2 == 0); 
    
    // //forall*, exists
    // // input_requirement = forall x . x>=0 && x < 100 : input(x) > 0;
    // // output_requirement = forall x . x>=0 && x < 100 : out(x) % 2 == 0 && out(x) > 0;

    // // even.context_perm(in(x), frac(1,2));
    // // even.context_perm(in(x+1), frac(1,2));
    // // even.require(in(x) % 2 == 0);
    // // even.require(in(x+1) % 2 == 0);
    // even(x) = in(x) + in(x+1);
    // //even.context_perm(even(x),frac(1,1));
    // even.ensure(even(x) % 2 == 0);

    // Var bx("bx"), tx("tx"), ix("ix"), xy("xy");

    // if(true){
    //     even.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
    //     even.bound(x,0,127);
    //     even.output_buffer().dim(0).set_min(0);

    //     in.in(even)
    //       .compute_at(even, bx)
    //       .gpu_threads(x);

    //     in.compute_root(); //Do not inline
    //     in.split(x, bx, tx, 32*2, TailStrategy::GuardWithIf);
    //     //in.unroll(tx, 2);
    //     in.split(tx, tx, ix, 2);
    //     in.gpu(bx, tx);
    //     in.update(0)
    //       .split(x, bx, tx, 32*4, TailStrategy::GuardWithIf)
    //       .unroll(tx, 2)
    //       .gpu(bx, tx)
    //       ;

    //     h.compute_at(in, bx).gpu_threads(x);
    //     h.update().reorder(r.x, x).gpu_threads(x);
        
    //     k.compute_root();


    //     f.compute_root()
    //      .fuse(x, y, xy)
    //      .split(xy, bx, tx, 32, TailStrategy::GuardWithIf)
    //      .gpu(bx, tx)
    //      ;

    //     f.update(0)
    //      .split(x, bx, tx, 32, TailStrategy::GuardWithIf)
    //      .gpu(bx, tx)
    //      ;

    //     //f.update(0).compute_root();

        
        

        
    // } else if(true) {
    //     even.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
    //     even.bound(x,0,127);
    //     even.output_buffer().dim(0).set_min(0);
    //     in.compute_at(even, bx);
    // } else {
    //     even.gpu_tile(x, bx, tx, 32, TailStrategy::GuardWithIf);
    //     even.bound(x,0,127);
    //     even.output_buffer().dim(0).set_min(0);
    // }
    
    // //in.gpu_tile(x, bx, tx, 2); // Divides extent
    // //even.gpu_tile(x, bx, tx, 3); // One thread block

    // Target target = get_host_target();
    // Target new_target = target
    //   .with_feature(Target::OpenCL)
    //   .with_feature(Target::NoAsserts)
    //   ;
    // assert(host_supports_target_device(new_target));
    // if(add_runtime){
    //     compile_standalone_runtime("opencl_runtime.o",new_target);
    //     even.compile_to_header("even_c.h", {}, "even", new_target);
    // }

    // if(!add_runtime && !aot)
    //     even.compile_to_c("even_c.cpp", {}, "even", new_target);

    // if(aot)
    //     even.compile_to_static_library("even_static", {}, "even", new_target);

    // if(false)
    // {
    //     //Sum try out
    //     Func f("f"), g("g"), g_int("g_int");
    //     Var i("i"), threads("threads"), blocks("blocks");
    //     RDom r(0, 256);
    //     f(i) = i*i*2;

    //     RVar r_blocks("r_blocks"), r_temp, r_threads("r_threads"), r_i("r_i");
    //     g() = 0;
    //     g() += f(r.x);

    //     f.compute_root();
    //     g.update()
    //         .split(r.x, r_blocks, r_temp, 32*4)
    //         .split(r_temp, r_threads, r_i, 4)
    //         ;
            
    //     g.update()
    //         .rfactor({{r_blocks, blocks}, {r_threads, threads}})
    //         // .gpu(blocks, threads)
    //         .compute_root()
    //         .update()
    //         .gpu(blocks, threads)
    //         ;

    //     g.compile_to_c("sum_c.cpp", {}, "sum", new_target);

    //     // Func histogram("hist_serial");
    //     // histogram(i) = 0;
    //     // RDom r(0, input.width(), 0, input.height());
    //     // histogram(input(r.x, r.y) / 32) += 1;

    // }
}