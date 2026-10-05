[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint2 dispatchThreadID: SV_DispatchThreadID, uint* data)
{
    data[dispatchThreadID.x] = dispatchThreadID.x;
}
