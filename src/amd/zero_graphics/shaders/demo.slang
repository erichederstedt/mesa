/* How to compile
slangc demo.slang -target spirv -profile spirv_1_5 -o demo.spv
xxd -i demo.spv > demo.h
*/

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint2 dispatchThreadID: SV_DispatchThreadID, uint* data)
{
    data[dispatchThreadID.x] = dispatchThreadID.x;
}
