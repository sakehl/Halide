from typing import Dict, Union

class Info:
    def __init__(self, name: str, version: str,
                 lines_of_code: int, nr_annotations: int, loops: int):
        self.name = name
        self.version = version
        self.lines_of_code = lines_of_code
        self.nr_annotations = nr_annotations
        self.loops = loops
    
    def __str__(self) -> str:
        return f"Name {self.name}-{self.version}\nCode: {self.lines_of_code}\nAnnotations:{self.nr_annotations}\nLoops: {self.loops}"

def is_annotation(line: str) -> bool:
    l = line.lstrip()
    return l.startswith("requires ") or l.startswith("ensures ") or \
        l.startswith("context ") or l.startswith("context_everywhere ") or \
        l.startswith("loop_invariant ")

def is_loop(line: str)-> bool:
    l = line.lstrip()
    return l.startswith("par ") or l.startswith("for ") or l.startswith("for(")

def is_given_func(line: str, given: str)-> bool:
    l = line.lstrip()
    return l.startswith("int " + given + "(")

def is_other_func(line: str) -> bool:
    l = line.lstrip()
    return l.startswith("pure ")

def is_none_line(line: str) -> bool:
    l = line.strip()
    return l == "" or l.startswith("{") or l.startswith("}")

def count_lines(name: str, version: str) -> Info:
    lines_of_code: int = 0
    nr_annotations: int = 0
    loops: int = 0
    pvl_name = 'build/' + name + '_pvl-' + version + '.pvl'
    with open(pvl_name) as f:
        for line in f:
            # skip empty lines or lines with only braces
            if is_none_line(line):
               continue
            # Skip helper functions 
            if is_other_func(line):
                lines_of_code = 0
                nr_annotations = 0
                loops = 0
                continue
            
            if is_annotation(line):
                nr_annotations += 1
            else:
                lines_of_code += 1
            
            if is_loop(line):
                loops += 1
    return Info(name, version, lines_of_code, nr_annotations, loops)