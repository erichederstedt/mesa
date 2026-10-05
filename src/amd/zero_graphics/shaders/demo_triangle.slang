struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
}

struct VOut
{
    float4 position : SV_Position;
    float3 normal : Normal;
    float2 uv : UV;
}

[shader("vertex")]
VOut vertexMain( uint vertexIndex: SV_VulkanVertexID, Vertex *input) // We essentially do vertex pulling only with our API. That way we can get rid of vertex buffers and input layouts etc.
{
    #if 0
    VOut output;
    output.position = float4(input[vertexIndex].position, 1.0);
    output.normal = input[vertexIndex].normal;
    output.uv = input[vertexIndex].uv;
    return output;
    #else
    VOut out;
    out.position = float4(0.0, 0.0, 0.0, 1.0);
    if (vertexIndex == 0) {
        out.position = float4(-0.5, -0.5, 0.0, 1.0);
    }
    if (vertexIndex == 1) {
        out.position = float4(0.5, -0.5, 0.0, 1.0);
    }
    if (vertexIndex == 2) {
        out.position = float4(0.0, 0.5, 0.0, 1.0);
    }
    return out;
    #endif
}

[shader("fragment")]
float4 fragmentMain(VOut input, float4 *extra_data) : SV_Target // Both vertex and pixel shader has a push constant each(This will change in the future but lets have this limitation for now).
{
    return float4(1.0, 0.0, 0.0, 1.0);
}