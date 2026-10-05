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
    VOut output;
    output.position = float4(input[vertexIndex].position, 1.0);
    output.normal = input[vertexIndex].normal;
    output.uv = input[vertexIndex].uv;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VOut input, float4 *extra_data) : SV_Target // Both vertex and pixel shader has a push constant each(This will change in the future but lets have this limitation for now).
{
    return *extra_data;
}