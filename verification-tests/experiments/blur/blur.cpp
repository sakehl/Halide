#include "Halide.h"
#include <stdio.h>

using namespace Halide;

int main(int argc, char *argv[]) {
  int schedule;
  std::string front_s = "front";
  bool front = false;
  if(argc == 1){
    schedule = 0;
  } else if(argc == 2){
    if(front_s.compare(argv[1]) == 0){
      front = true;
    } else {
      schedule = std::stoi(argv[1]);
    }
  }
  if((argc > 2 || schedule < 0 || schedule >3) && !front){
    printf("Invallid argument\n");
    return 1;
  }

  ImageParam input(type_of<int>(), 2, "input");

  Func blur_x("blur_x"), blur_y("blur_y");
  Var x("x"), y("y"), xi("xi"), yi("yi"), yo("yo"), yoo("yoo") ;

  input.requires(input(_0, _1) >= 0);
  input.requires(input(_0, _1) < 256);

  blur_x(x, y) = 
    (input(x, y) + input(x + 1, y) + input(x + 2, y)) / 3;
  // blur_x.ensures(blur_x(x, y) ==
  //   (input(x, y) + input(x + 1, y) + input(x + 2, y)) / 3);
  blur_x.ensures(blur_x(x, y) >= 0);
  blur_x.ensures(blur_x(x, y) < 256);

  blur_y(x, y) = 
    (blur_x(x, y) + blur_x(x, y + 1) + blur_x(x, y + 2)) / 3;
  blur_y.ensures(blur_y(x, y) >= 0);
  blur_y.ensures(blur_y(x, y) < 256);
  // blur_y.ensures(blur_y(x, y) ==  
  //       ((input(x, y)   + input(x+1, y)   + input(x+2, y))/3
  //     + (input(x, y+1) + input(x+1, y+1) + input(x+2, y+1))/3
  //     + (input(x, y+2) + input(x+1, y+2) + input(x+2, y+2))/3)/3
  //   );

  
  if(schedule == 0){

  } else if(schedule == 1) {
    blur_y.split(y, y, yi, 8, TailStrategy::GuardWithIf).unroll(x, 8);
    blur_x.compute_at(blur_y, yi).unroll(x, 8);
  } else if(schedule == 2) {
    blur_y.split(y, y, yi, 8, TailStrategy::GuardWithIf).parallel(y);
    blur_x.store_at(blur_y, y).compute_at(blur_y, yi);
  } else if(schedule == 3) {
    Var xy("xy");
    blur_y.split(y, y, yi, 8, TailStrategy::GuardWithIf)
      .split(x, x, xi, 8, TailStrategy::GuardWithIf)
      .reorder(xi, yi, x, y)
      // .fuse(x, y, xy).parallel(xy)
      .parallel(x)
      .parallel(y)
      ;
    blur_x.compute_at(blur_y, x);
  }

  int n = 1024;
  blur_y.output_buffer().dim(0).set_bounds(0,n); 
  blur_y.output_buffer().dim(1).set_bounds(0,n);
  blur_y.output_buffer().dim(1).set_stride(n);
  
  input.dim(0).set_bounds(0,n+2);
  input.dim(1).set_bounds(0,n+2);
  input.dim(1).set_stride(n+2);

  Target target = Target();
  Target new_target = target
    .with_feature(Target::NoAsserts)
    .with_feature(Target::NoBoundsQuery)
    ;

  if(front) {
    blur_y.translate_to_pvl("blur_front_pvl.pvl", {}); 
  } else {
    blur_y.compile_to_pvl("blur_pvl-" + std::to_string(schedule) + ".pvl" , {input}, "blur", new_target);
  }
}