# Demo Shaders
## How to compile
slangc file_name.slang -target spirv -profile spirv_1_5 -o file_name.spv
xxd -i file_name.spv > file_name.h
