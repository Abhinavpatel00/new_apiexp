// mu_mesh.slang — the vertex and object records of demos/common/mesh.h, and
// column-vector matrices.
//
// Matrices are stored as four float4 columns (mu_math is column-major) and
// multiplied by hand. That keeps the byte layout obvious on both sides and
// independent of Slang's matrix layout mode: `muMul(m, v)` is `M * v`.
module mu_mesh;
import mu_gfx;

public struct MuMat4
{
    public float4 c0, c1, c2, c3;
};
public float4 muMul(MuMat4 m, float4 v) { return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z + m.c3 * v.w; }
public float3 muMulPoint(MuMat4 m, float3 p) { return (m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3).xyz; }
public float3 muMulDir(MuMat4 m, float3 d) { return (m.c0 * d.x + m.c1 * d.y + m.c2 * d.z).xyz; }
public MuMat4 muLoadMat4(uint offset)
{
    MuMat4 m;
    m.c0 = muScene.Load<float4>(offset);
    m.c1 = muScene.Load<float4>(offset + 16u);
    m.c2 = muScene.Load<float4>(offset + 32u);
    m.c3 = muScene.Load<float4>(offset + 48u);
    return m;
}

public static const uint MU_MESH_VERTEX_SIZE = 32u;
public static const uint MU_MESH_OBJECT_SIZE = 96u;

public struct MeshVertex
{
    public float3 position;
    public float3 normal;
};
/// `offset` is the byte offset of vertex 0 of the draw. Draw with
/// first_vertex = 0 and put the start in the offset: Slang's SV_VertexID does
/// not include the base vertex.
public MeshVertex muLoadMeshVertex(uint offset, uint index)
{
    MeshVertex v;
    v.position = muScene.Load<float4>(offset + index * MU_MESH_VERTEX_SIZE).xyz;
    v.normal   = muScene.Load<float4>(offset + index * MU_MESH_VERTEX_SIZE + 16u).xyz;
    return v;
}

public struct MeshObject
{
    public MuMat4 model;
    public float4 color;
    public float4 params;
};
public MeshObject muLoadMeshObject(uint offset)
{
    MeshObject o;
    o.model  = muLoadMat4(offset);
    o.color  = muScene.Load<float4>(offset + 64u);
    o.params = muScene.Load<float4>(offset + 80u);
    return o;
}
