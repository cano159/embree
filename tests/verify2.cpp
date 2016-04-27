// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "verify2.h"

#define DEFAULT_STACK_SIZE 4*1024*1024

#if defined(__WIN32__)
#  define GREEN(x) x
#  define RED(x) x
#else
#  define GREEN(x) "\033[32m" x "\033[0m"
#  define RED(x) "\033[31m" x "\033[0m"
#endif

#if defined(RTCORE_RAY_PACKETS) && !defined(__MIC__)
#  define HAS_INTERSECT4 1
#else
#  define HAS_INTERSECT4 0
#endif

#if defined(RTCORE_RAY_PACKETS) && (defined(__TARGET_AVX__) || defined(__TARGET_AVX2__))
#  define HAS_INTERSECT8 1
#else
#  define HAS_INTERSECT8 0
#endif

#if defined(RTCORE_RAY_PACKETS) && (defined(__MIC__) || defined(__TARGET_AVX512KNL__))
#  define HAS_INTERSECT16 1
#else
#  define HAS_INTERSECT16 0
#endif

#if defined(__INTEL_COMPILER)
#pragma warning (disable: 1478) // warning: function was declared deprecated
#elif defined(_MSC_VER)
#pragma warning (disable: 4996) // warning: function was declared deprecated
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // warning: xxx is deprecated
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // warning: xxx is deprecated
#endif

namespace embree
{
  bool hasISA(const int isa) 
  {
    int cpu_features = getCPUFeatures();
    return (cpu_features & isa) == isa;
  }

  /* vertex and triangle layout */
  struct Vertex  {
    Vertex() {}
    Vertex(float x, float y, float z, float a = 0.0f) 
      : x(x), y(y), z(z), a(a) {}
    float x,y,z,a; 
  };
  typedef Vec3f  Vertex3f;
  typedef Vec3fa Vertex3fa;
  
  struct Triangle {
    Triangle () {}
    Triangle(int v0, int v1, int v2) : v0(v0), v1(v1), v2(v2) {}
    int v0, v1, v2; 
  };

  std::vector<void*> buffers;
  MutexSys g_mutex2;
  void* allocBuffer(size_t size) { 
    g_mutex2.lock();
    void* ptr = alignedMalloc(size);
    buffers.push_back(ptr); 
    g_mutex2.unlock();
    return ptr; 
  }
  void clearBuffers() {
    for (size_t i=0; i<buffers.size(); i++) {
      alignedFree(buffers[i]);
    }
    buffers.clear();
  }
  struct ClearBuffers {
    ~ClearBuffers() { clearBuffers(); }
  };

  const size_t numSceneFlags = 64;

  RTCSceneFlags getSceneFlag(size_t i) 
  {
    int flag = 0;                               
    if (i & 1) flag |= RTC_SCENE_DYNAMIC;
    if (i & 2) flag |= RTC_SCENE_COMPACT;
    if (i & 4) flag |= RTC_SCENE_COHERENT;
    if (i & 8) flag |= RTC_SCENE_INCOHERENT;
    if (i & 16) flag |= RTC_SCENE_HIGH_QUALITY;
    if (i & 32) flag |= RTC_SCENE_ROBUST;
    return (RTCSceneFlags) flag;
  }

  const size_t numSceneGeomFlags = 32;

  void getSceneGeomFlag(size_t i, RTCSceneFlags& sflags, RTCGeometryFlags& gflags) 
  {
    int sflag = 0, gflag = 0;
    if (i & 4) {
      sflag |= RTC_SCENE_DYNAMIC;
      gflag = min(i&3,size_t(2));
    }
    if (i & 8) sflag |= RTC_SCENE_HIGH_QUALITY;
    if (i & 16) sflag |= RTC_SCENE_ROBUST;
    sflags = (RTCSceneFlags) sflag;
    gflags = (RTCGeometryFlags) gflag;
  }

  RTCRay makeRay(const Vec3fa& org, const Vec3fa& dir) 
  {
    RTCRay ray;
    ray.org[0] = org.x; ray.org[1] = org.y; ray.org[2] = org.z;
    ray.dir[0] = dir.x; ray.dir[1] = dir.y; ray.dir[2] = dir.z;
    ray.tnear = 0.0f; ray.tfar = inf;
    ray.time = 0; ray.mask = -1;
    ray.geomID = ray.primID = ray.instID = -1;
    return ray;
  }

  RTCRay makeRay(const Vec3fa& org, const Vec3fa& dir, float tnear, float tfar) 
  {
    RTCRay ray;
    ray.org[0] = org.x; ray.org[1] = org.y; ray.org[2] = org.z;
    ray.dir[0] = dir.x; ray.dir[1] = dir.y; ray.dir[2] = dir.z;
    ray.tnear = tnear; ray.tfar = tfar;
    ray.time = 0; ray.mask = -1;
    ray.geomID = ray.primID = ray.instID = -1;
    return ray;
  }
  
  void setRay(RTCRay4& ray_o, int i, const RTCRay& ray_i)
  {
    ray_o.orgx[i] = ray_i.org[0];
    ray_o.orgy[i] = ray_i.org[1];
    ray_o.orgz[i] = ray_i.org[2];
    ray_o.dirx[i] = ray_i.dir[0];
    ray_o.diry[i] = ray_i.dir[1];
    ray_o.dirz[i] = ray_i.dir[2];
    ray_o.tnear[i] = ray_i.tnear;
    ray_o.tfar[i] = ray_i.tfar;
    ray_o.Ngx[i] = ray_i.Ng[0];
    ray_o.Ngy[i] = ray_i.Ng[1];
    ray_o.Ngz[i] = ray_i.Ng[2];
    ray_o.time[i] = ray_i.time;
    ray_o.mask[i] = ray_i.mask;
    ray_o.geomID[i] = ray_i.geomID;
    ray_o.primID[i] = ray_i.primID;
    ray_o.instID[i] = ray_i.instID;
  }

  void setRay(RTCRay8& ray_o, int i, const RTCRay& ray_i)
  {
    ray_o.orgx[i] = ray_i.org[0];
    ray_o.orgy[i] = ray_i.org[1];
    ray_o.orgz[i] = ray_i.org[2];
    ray_o.dirx[i] = ray_i.dir[0];
    ray_o.diry[i] = ray_i.dir[1];
    ray_o.dirz[i] = ray_i.dir[2];
    ray_o.tnear[i] = ray_i.tnear;
    ray_o.tfar[i] = ray_i.tfar;
    ray_o.Ngx[i] = ray_i.Ng[0];
    ray_o.Ngy[i] = ray_i.Ng[1];
    ray_o.Ngz[i] = ray_i.Ng[2];
    ray_o.time[i] = ray_i.time;
    ray_o.mask[i] = ray_i.mask;
    ray_o.geomID[i] = ray_i.geomID;
    ray_o.primID[i] = ray_i.primID;
    ray_o.instID[i] = ray_i.instID;
  }

  void setRay(RTCRay16& ray_o, int i, const RTCRay& ray_i)
  {
    ray_o.orgx[i] = ray_i.org[0];
    ray_o.orgy[i] = ray_i.org[1];
    ray_o.orgz[i] = ray_i.org[2];
    ray_o.dirx[i] = ray_i.dir[0];
    ray_o.diry[i] = ray_i.dir[1];
    ray_o.dirz[i] = ray_i.dir[2];
    ray_o.tnear[i] = ray_i.tnear;
    ray_o.tfar[i] = ray_i.tfar;
    ray_o.Ngx[i] = ray_i.Ng[0];
    ray_o.Ngy[i] = ray_i.Ng[1];
    ray_o.Ngz[i] = ray_i.Ng[2];
    ray_o.time[i] = ray_i.time;
    ray_o.mask[i] = ray_i.mask;
    ray_o.geomID[i] = ray_i.geomID;
    ray_o.primID[i] = ray_i.primID;
    ray_o.instID[i] = ray_i.instID;
  }

  RTCRay getRay(RTCRay4& ray_i, int i)
  {
    RTCRay ray_o;
    ray_o.org[0] = ray_i.orgx[i];
    ray_o.org[1] = ray_i.orgy[i];
    ray_o.org[2] = ray_i.orgz[i];
    ray_o.dir[0] = ray_i.dirx[i];
    ray_o.dir[1] = ray_i.diry[i];
    ray_o.dir[2] = ray_i.dirz[i];
    ray_o.tnear = ray_i.tnear[i];
    ray_o.tfar = ray_i.tfar[i];
    ray_o.Ng[0] = ray_i.Ngx[i];
    ray_o.Ng[1] = ray_i.Ngy[i];
    ray_o.Ng[2] = ray_i.Ngz[i];
    ray_o.time = ray_i.time[i];
    ray_o.mask = ray_i.mask[i];
    ray_o.geomID = ray_i.geomID[i];
    ray_o.primID = ray_i.primID[i];
    ray_o.instID = ray_i.instID[i];
    return ray_o;
  }

  RTCRay getRay(RTCRay8& ray_i, int i)
  {
    RTCRay ray_o;
    ray_o.org[0] = ray_i.orgx[i];
    ray_o.org[1] = ray_i.orgy[i];
    ray_o.org[2] = ray_i.orgz[i];
    ray_o.dir[0] = ray_i.dirx[i];
    ray_o.dir[1] = ray_i.diry[i];
    ray_o.dir[2] = ray_i.dirz[i];
    ray_o.tnear = ray_i.tnear[i];
    ray_o.tfar = ray_i.tfar[i];
    ray_o.Ng[0] = ray_i.Ngx[i];
    ray_o.Ng[1] = ray_i.Ngy[i];
    ray_o.Ng[2] = ray_i.Ngz[i];
    ray_o.time = ray_i.time[i];
    ray_o.mask = ray_i.mask[i];
    ray_o.geomID = ray_i.geomID[i];
    ray_o.primID = ray_i.primID[i];
    ray_o.instID = ray_i.instID[i];
    return ray_o;
  }

  RTCRay getRay(RTCRay16& ray_i, int i)
  {
    RTCRay ray_o;
    ray_o.org[0] = ray_i.orgx[i];
    ray_o.org[1] = ray_i.orgy[i];
    ray_o.org[2] = ray_i.orgz[i];
    ray_o.dir[0] = ray_i.dirx[i];
    ray_o.dir[1] = ray_i.diry[i];
    ray_o.dir[2] = ray_i.dirz[i];
    ray_o.tnear = ray_i.tnear[i];
    ray_o.tfar = ray_i.tfar[i];
    ray_o.Ng[0] = ray_i.Ngx[i];
    ray_o.Ng[1] = ray_i.Ngy[i];
    ray_o.Ng[2] = ray_i.Ngz[i];
    ray_o.time = ray_i.time[i];
    ray_o.mask = ray_i.mask[i];
    ray_o.geomID = ray_i.geomID[i];
    ray_o.primID = ray_i.primID[i];
    ray_o.instID = ray_i.instID[i];
    return ray_o;
  }

  void rtcIntersectN(const RTCSceneRef& scene, RTCRay& ray, int N) 
  {
    switch (N) {
    case 1: {
      rtcIntersect(scene,ray); 
      break;
    }
#if HAS_INTERSECT4
    case 4: {
      RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
      for (size_t i=0; i<4; i++) setRay(ray4,i,ray);
      __aligned(16) int valid[4] = { -1,-1,-1,-1 };
      rtcIntersect4(valid,scene,ray4);
      ray = getRay(ray4,0);
      break;
    }
#endif
#if HAS_INTERSECT8
    case 8: {
      RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
      for (size_t i=0; i<8; i++) setRay(ray8,i,ray);
      __aligned(32) int valid[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
      rtcIntersect8(valid,scene,ray8);
      ray = getRay(ray8,0);
      break;
    }
#endif
#if HAS_INTERSECT16
    case 16: {
      RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
      for (size_t i=0; i<16; i++) setRay(ray16,i,ray);
      __aligned(64) int valid[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
      rtcIntersect16(valid,scene,ray16);
      ray = getRay(ray16,0);
      break;
    }
#endif
    default: break;
    }
  }

  void rtcOccludedN(const RTCSceneRef& scene, RTCRay& ray, int N) 
  {
    switch (N) {
    case 1: {
      rtcOccluded(scene,ray); 
      break;
    }
#if HAS_INTERSECT4
    case 4: {
      RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
      for (size_t i=0; i<4; i++) setRay(ray4,i,ray);
      __aligned(16) int valid[4] = { -1,-1,-1,-1 };
      rtcOccluded4(valid,scene,ray4);
      ray.geomID = ray4.geomID[0];
      break;
    }
#endif
#if HAS_INTERSECT8
    case 8: {
      RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
      for (size_t i=0; i<8; i++) setRay(ray8,i,ray);
      __aligned(32) int valid[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
      rtcOccluded8(valid,scene,ray8);
      ray.geomID = ray8.geomID[0];
      break;
    }
#endif
#if HAS_INTERSECT16
    case 16: {
      RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
      for (size_t i=0; i<16; i++) setRay(ray16,i,ray);
      __aligned(64) int valid[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
      rtcOccluded16(valid,scene,ray16);
      ray.geomID = ray16.geomID[0];
      break;
    }
#endif
    default: break;
    }
  }

  void AssertNoError(RTCDevice device) 
  {
    RTCError error = rtcDeviceGetError(device);
    if (error != RTC_NO_ERROR) 
      throw std::runtime_error("Error occured: "+string_of(error));
  }

  void AssertAnyError(RTCDevice device)
  {
    RTCError error = rtcDeviceGetError(device);
    if (error == RTC_NO_ERROR) 
      throw std::runtime_error("Any error expected");
  }

  void AssertError(RTCDevice device, RTCError expectedError)
  {
    RTCError error = rtcDeviceGetError(device);
    if (error != expectedError) 
      throw std::runtime_error("Error "+string_of(expectedError)+" expected");
  }

  RTCAlgorithmFlags aflags = (RTCAlgorithmFlags) (RTC_INTERSECT1 | RTC_INTERSECT4 | RTC_INTERSECT8 | RTC_INTERSECT16);

  bool g_enable_build_cancel = false;

  unsigned addPlane (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, size_t num, const Vec3fa& p0, const Vec3fa& dx, const Vec3fa& dy)
  {
    unsigned mesh = rtcNewTriangleMesh (scene, flag, 2*num*num, (num+1)*(num+1));
    Vertex3fa*   vertices  = (Vertex3fa*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    Triangle* triangles = (Triangle*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    for (size_t y=0; y<=num; y++) {
      for (size_t x=0; x<=num; x++) {
        Vec3fa p = p0+float(x)/float(num)*dx+float(y)/float(num)*dy;
        size_t i = y*(num+1)+x;
        vertices[i].x = p.x;
        vertices[i].y = p.y;
        vertices[i].z = p.z;
      }
    }
    for (size_t y=0; y<num; y++) {
      for (size_t x=0; x<num; x++) {
        size_t i = 2*y*num+2*x;
        size_t p00 = (y+0)*(num+1)+(x+0);
        size_t p01 = (y+0)*(num+1)+(x+1);
        size_t p10 = (y+1)*(num+1)+(x+0);
        size_t p11 = (y+1)*(num+1)+(x+1);
        triangles[i+0].v0 = p01; triangles[i+0].v1 = p00; triangles[i+0].v2 = p11;
        triangles[i+1].v0 = p10; triangles[i+1].v1 = p11; triangles[i+1].v2 = p00;
      }
    }
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    return mesh;
  }

  unsigned addSubdivPlane (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, size_t num, const Vec3fa& p0, const Vec3fa& dx, const Vec3fa& dy)
  {
    unsigned mesh = rtcNewSubdivisionMesh (scene, flag, num*num, 4*num*num, (num+1)*(num+1), 0,0,0);
    Vertex3fa*   vertices  = (Vertex3fa*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    int* indices = (int*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    int* faces = (int*) rtcMapBuffer(scene,mesh,RTC_FACE_BUFFER);
    for (size_t y=0; y<=num; y++) {
      for (size_t x=0; x<=num; x++) {
        Vec3fa p = p0+float(x)/float(num)*dx+float(y)/float(num)*dy;
        size_t i = y*(num+1)+x;
        vertices[i].x = p.x;
        vertices[i].y = p.y;
        vertices[i].z = p.z;
      }
    }
    for (size_t y=0; y<num; y++) {
      for (size_t x=0; x<num; x++) {
        size_t i = y*num+x;
        size_t p00 = (y+0)*(num+1)+(x+0);
        size_t p01 = (y+0)*(num+1)+(x+1);
        size_t p10 = (y+1)*(num+1)+(x+0);
        size_t p11 = (y+1)*(num+1)+(x+1);
        indices[4*i+0] = p00; 
        indices[4*i+1] = p01; 
        indices[4*i+2] = p11; 
        indices[4*i+3] = p10; 
        faces[i] = 4;
      }
    }
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    rtcUnmapBuffer(scene,mesh,RTC_FACE_BUFFER);
    rtcSetBoundaryMode(scene,mesh,RTC_BOUNDARY_EDGE_AND_CORNER);
    return mesh;
  }

  unsigned addSphere (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, const Vec3fa& pos, const float r, size_t numPhi, size_t maxTriangles = -1, float motion = 0.0f, BBox3fa* bounds_o = nullptr)
  {
    /* create a triangulated sphere */
    size_t numTheta = 2*numPhi;
    size_t numTriangles = min(maxTriangles,2*numTheta*(numPhi-1));
    size_t numTimeSteps = motion == 0.0f ? 1 : 2;
    size_t numVertices = numTheta*(numPhi+1);
    
    unsigned mesh = rtcNewTriangleMesh (scene, flag, numTriangles, numVertices,numTimeSteps);
    
    /* map triangle and vertex buffer */
    Vertex3f* vertices0 = nullptr;
    Vertex3f* vertices1 = nullptr;
    if (numTimeSteps >= 1) rtcSetBuffer(scene,mesh,RTC_VERTEX_BUFFER0,vertices0 = (Vertex3f*) allocBuffer(numVertices*sizeof(Vertex3f)), 0, sizeof(Vertex3f)); 
    if (numTimeSteps >= 2) rtcSetBuffer(scene,mesh,RTC_VERTEX_BUFFER1,vertices1 = (Vertex3f*) allocBuffer(numVertices*sizeof(Vertex3f)), 0, sizeof(Vertex3f)); 
    Triangle* triangles = (Triangle*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }

    /* create sphere geometry */
    BBox3fa bounds = empty;
    size_t tri = 0;
    const float rcpNumTheta = 1.0f/float(numTheta);
    const float rcpNumPhi   = 1.0f/float(numPhi);
    for (size_t phi=0; phi<=numPhi; phi++)
    {
      for (size_t theta=0; theta<numTheta; theta++)
      {
        const float phif   = phi*float(pi)*rcpNumPhi;
        const float thetaf = theta*2.0f*float(pi)*rcpNumTheta;
        Vertex3f* v = &vertices0[phi*numTheta+theta];
        const float cosThetaf = cos(thetaf);
        v->x = pos.x + r*sin(phif)*sin(thetaf);
        v->y = pos.y + r*cos(phif);
        v->z = pos.z + r*sin(phif)*cosThetaf;
        bounds.extend(Vec3fa(v->x,v->y,v->z));

        if (vertices1) {
          Vertex3f* v1 = &vertices1[phi*numTheta+theta];
          const float cosThetaf = cos(thetaf);
          v1->x = motion + pos.x + r*sin(phif)*sin(thetaf);
          v1->y = motion + pos.y + r*cos(phif);
          v1->z = motion + pos.z + r*sin(phif)*cosThetaf;
          bounds.extend(Vec3fa(v1->x,v1->y,v1->z));
        }
      }
      if (phi == 0) continue;

      for (size_t theta=1; theta<=numTheta; theta++) 
      {
        int p00 = (phi-1)*numTheta+theta-1;
        int p01 = (phi-1)*numTheta+theta%numTheta;
        int p10 = phi*numTheta+theta-1;
        int p11 = phi*numTheta+theta%numTheta;
        
        if (phi > 1) {
          if (tri < numTriangles) {
            triangles[tri].v0 = p10; 
            triangles[tri].v1 = p00; 
            triangles[tri].v2 = p01; 
            tri++;
          }
        }
        
        if (phi < numPhi) {
          if (tri < numTriangles) {
            triangles[tri].v0 = p11; 
            triangles[tri].v1 = p10;
            triangles[tri].v2 = p01; 
            tri++;
          }
        }
      }
    }

    //if (numTimeSteps >= 1) rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER0); 
    //if (numTimeSteps >= 2) rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER1); 
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);

    if (bounds_o) *bounds_o = bounds;
    return mesh;
  }

  /* adds a subdiv sphere to the scene */
  unsigned int addSubdivSphere (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flags, const Vec3fa& pos, const float r, size_t numPhi, float level, size_t maxFaces = -1, float motion = 0.0f)
  {
    size_t numTheta = 2*numPhi;
    avector<Vec3fa> vertices(numTheta*(numPhi+1));
    std::vector<int> indices;
    std::vector<int> faces;
    std::vector<int> offsets;
    
    /* create sphere geometry */
    const float rcpNumTheta = rcp((float)numTheta);
    const float rcpNumPhi   = rcp((float)numPhi);
    for (int phi=0; phi<=numPhi; phi++)
    {
      for (int theta=0; theta<numTheta; theta++)
      {
	const float phif   = phi*float(pi)*rcpNumPhi;
	const float thetaf = theta*2.0f*float(pi)*rcpNumTheta;
	Vec3fa& v = vertices[phi*numTheta+theta];
	Vec3fa P(pos.x + r*sin(phif)*sin(thetaf),
		 pos.y + r*cos(phif),
		 pos.z + r*sin(phif)*cos(thetaf));
	v.x = P.x;
	v.y = P.y;
	v.z = P.z;
      }
      if (phi == 0) continue;
      
      if (phi == 1)
      {
	for (int theta=1; theta<=numTheta; theta++) 
	{
	  int p00 = numTheta-1;
	  int p10 = phi*numTheta+theta-1;
	  int p11 = phi*numTheta+theta%numTheta;
	  offsets.push_back(indices.size());
	  indices.push_back(p10); 
	  indices.push_back(p00);
	  indices.push_back(p11);
	  faces.push_back(3);
	}
      }
      else if (phi == numPhi)
      {
	for (int theta=1; theta<=numTheta; theta++) 
	{
	  int p00 = (phi-1)*numTheta+theta-1;
	  int p01 = (phi-1)*numTheta+theta%numTheta;
	  int p10 = numPhi*numTheta;
	  offsets.push_back(indices.size());
	  indices.push_back(p10);
	  indices.push_back(p00);
	  indices.push_back(p01);
	  faces.push_back(3);
	}
      }
      else
      {
	for (int theta=1; theta<=numTheta; theta++) 
	{
	  int p00 = (phi-1)*numTheta+theta-1;
	  int p01 = (phi-1)*numTheta+theta%numTheta;
	  int p10 = phi*numTheta+theta-1;
	  int p11 = phi*numTheta+theta%numTheta;
	  offsets.push_back(indices.size());
	  indices.push_back(p10);
	  indices.push_back(p00);
	  indices.push_back(p01);
	  indices.push_back(p11);
	  faces.push_back(4);
	}
      }
    }
    
    /* create subdiv geometry */
    size_t numFaces = min(faces.size(),maxFaces);
    size_t numEdges = indices.size();
    size_t numVertices = vertices.size();
    size_t numEdgeCreases = 10;
    size_t numVertexCreases = 10;
    size_t numHoles = 0; // do not test holes as this causes some tests that assume a closed sphere to fail
    unsigned int mesh = rtcNewSubdivisionMesh(scene, flags, numFaces, numEdges, numVertices, numEdgeCreases, numVertexCreases, numHoles);
    Vec3fa* vertexBuffer = (Vec3fa*  ) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER);  if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }
    int*    indexBuffer  = (int     *) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);   if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }
    int*    facesBuffer = (int     *) rtcMapBuffer(scene,mesh,RTC_FACE_BUFFER);     if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }
    float*  levelBuffer  = (float   *) rtcMapBuffer(scene,mesh,RTC_LEVEL_BUFFER);   if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }

    memcpy(vertexBuffer,vertices.data(),numVertices*sizeof(Vec3fa));
    memcpy(indexBuffer ,indices.data() ,numEdges*sizeof(int));
    memcpy(facesBuffer,faces.data() ,numFaces*sizeof(int));
    for (size_t i=0; i<indices.size(); i++) levelBuffer[i] = level;
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    rtcUnmapBuffer(scene,mesh,RTC_FACE_BUFFER);
    rtcUnmapBuffer(scene,mesh,RTC_LEVEL_BUFFER);
    
    int* edgeCreaseIndices  = (int*) rtcMapBuffer(scene,mesh,RTC_EDGE_CREASE_INDEX_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }
    float* edgeCreaseWeights = (float*) rtcMapBuffer(scene,mesh,RTC_EDGE_CREASE_WEIGHT_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }

    for (size_t i=0; i<numEdgeCreases; i++) 
    {
      if (faces.size()) {
	int f = random<int>() % faces.size();
	int n = faces[f];
	int e = random<int>() % n;
	edgeCreaseIndices[2*i+0] = indices[offsets[f]+(e+0)%n];
	edgeCreaseIndices[2*i+1] = indices[offsets[f]+(e+1)%n];
      } else {
	edgeCreaseIndices[2*i+0] = 0;
	edgeCreaseIndices[2*i+1] = 0;
      }
      edgeCreaseWeights[i] = 10.0f*drand48();
    }
    rtcUnmapBuffer(scene,mesh,RTC_EDGE_CREASE_INDEX_BUFFER); 
    rtcUnmapBuffer(scene,mesh,RTC_EDGE_CREASE_WEIGHT_BUFFER); 
    
    int* vertexCreaseIndices  = (int*) rtcMapBuffer(scene,mesh,RTC_VERTEX_CREASE_INDEX_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }
    float* vertexCreaseWeights = (float*) rtcMapBuffer(scene,mesh,RTC_VERTEX_CREASE_WEIGHT_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,mesh); return -1; }

    for (size_t i=0; i<numVertexCreases; i++) 
    {
      int v = numTheta-1 + random<int>() % (vertices.size()+2-2*numTheta);
      vertexCreaseIndices[i] = v;
      vertexCreaseWeights[i] = 10.0f*drand48();
    }
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_CREASE_INDEX_BUFFER); 
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_CREASE_WEIGHT_BUFFER); 
    
    int* holeBuffer  = (int*) rtcMapBuffer(scene,mesh,RTC_HOLE_BUFFER);
    for (size_t i=0; i<numHoles; i++) {
      holeBuffer[i] = random<int>() % faces.size();
    }
    rtcUnmapBuffer(scene,mesh,RTC_HOLE_BUFFER); 
    
    return mesh;
  }

  unsigned int addCube (RTCDevice g_device, const RTCSceneRef& scene_i, RTCGeometryFlags flag, const Vec3fa& pos, const float r)
  {
    /* create a triangulated cube with 12 triangles and 8 vertices */
    unsigned int mesh = rtcNewTriangleMesh (scene_i, flag, 12, 8);
    
    /* set vertices */
    Vec3fa* vertices = (Vec3fa*) rtcMapBuffer(scene_i,mesh,RTC_VERTEX_BUFFER); 
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene_i,mesh); return -1; }
    vertices[0] = pos + r*Vec3fa(-1,-1,-1); 
    vertices[1] = pos + r*Vec3fa(-1,-1,+1); 
    vertices[2] = pos + r*Vec3fa(-1,+1,-1); 
    vertices[3] = pos + r*Vec3fa(-1,+1,+1); 
    vertices[4] = pos + r*Vec3fa(+1,-1,-1); 
    vertices[5] = pos + r*Vec3fa(+1,-1,+1); 
    vertices[6] = pos + r*Vec3fa(+1,+1,-1); 
    vertices[7] = pos + r*Vec3fa(+1,+1,+1); 
    rtcUnmapBuffer(scene_i,mesh,RTC_VERTEX_BUFFER); 

    /* set triangles and colors */
    int tri = 0;
    Triangle* triangles = (Triangle*) rtcMapBuffer(scene_i,mesh,RTC_INDEX_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene_i,mesh); return -1; }
    
    // left side
    triangles[tri].v0 = 0; triangles[tri].v1 = 2; triangles[tri].v2 = 1; tri++;
    triangles[tri].v0 = 1; triangles[tri].v1 = 2; triangles[tri].v2 = 3; tri++;

    // right side
    triangles[tri].v0 = 4; triangles[tri].v1 = 5; triangles[tri].v2 = 6; tri++;
    triangles[tri].v0 = 5; triangles[tri].v1 = 7; triangles[tri].v2 = 6; tri++;
    
    // bottom side
    triangles[tri].v0 = 0; triangles[tri].v1 = 1; triangles[tri].v2 = 4; tri++;
    triangles[tri].v0 = 1; triangles[tri].v1 = 5; triangles[tri].v2 = 4; tri++;
    
    // top side
    triangles[tri].v0 = 2; triangles[tri].v1 = 6; triangles[tri].v2 = 3; tri++;
    triangles[tri].v0 = 3; triangles[tri].v1 = 6; triangles[tri].v2 = 7; tri++;
    
    // front side
    triangles[tri].v0 = 0; triangles[tri].v1 = 4; triangles[tri].v2 = 2; tri++;
    triangles[tri].v0 = 2; triangles[tri].v1 = 4; triangles[tri].v2 = 6; tri++;
    
    // back side
    triangles[tri].v0 = 1; triangles[tri].v1 = 3; triangles[tri].v2 = 5; tri++;
    triangles[tri].v0 = 3; triangles[tri].v1 = 7; triangles[tri].v2 = 5; tri++;
    
    rtcUnmapBuffer(scene_i,mesh,RTC_INDEX_BUFFER);
    
    return mesh;
  }

  unsigned addHair (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, const Vec3fa& pos, const float scale, const float r, size_t numHairs = 1, float motion = 0.0f)
  {
    size_t numTimeSteps = motion == 0.0f ? 1 : 2;
    unsigned geomID = rtcNewHairGeometry (scene, flag, numHairs, numHairs*4, numTimeSteps);
    
    /* map triangle and vertex buffer */
    Vec3fa* vertices0 = nullptr;
    Vec3fa* vertices1 = nullptr;
    if (numTimeSteps >= 1) {
      vertices0 = (Vec3fa*) rtcMapBuffer(scene,geomID,RTC_VERTEX_BUFFER0); 
      if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,geomID); return -1; }
    }
    if (numTimeSteps >= 2) {
      vertices1 = (Vec3fa*) rtcMapBuffer(scene,geomID,RTC_VERTEX_BUFFER1); 
      if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,geomID); return -1; }
    }
    int* indices = (int*) rtcMapBuffer(scene,geomID,RTC_INDEX_BUFFER);
    if (rtcDeviceGetError(g_device) != RTC_NO_ERROR) { rtcDeleteGeometry(scene,geomID); return -1; }

    for (size_t i=0; i<numHairs; i++) 
    {
      indices[i] = 4*i;
      const Vec3fa p0 = pos + scale*Vec3fa(i%7,i%13,i%31);
      const Vec3fa p1 = p0 + scale*Vec3fa(1,0,0);
      const Vec3fa p2 = p0 + scale*Vec3fa(0,1,1);
      const Vec3fa p3 = p0 + scale*Vec3fa(0,1,0);
      
      if (vertices0) {
        vertices0[4*i+0] = Vec3fa(p0,r);
        vertices0[4*i+1] = Vec3fa(p1,r);
        vertices0[4*i+2] = Vec3fa(p2,r);
        vertices0[4*i+3] = Vec3fa(p3,r);
      }
      if (vertices1) {
        vertices1[4*i+0] = Vec3fa(p0+Vec3fa(motion),r);
        vertices1[4*i+1] = Vec3fa(p1+Vec3fa(motion),r);
        vertices1[4*i+2] = Vec3fa(p2+Vec3fa(motion),r);
        vertices1[4*i+3] = Vec3fa(p3+Vec3fa(motion),r);
      }
    }

    if (numTimeSteps >= 1) rtcUnmapBuffer(scene,geomID,RTC_VERTEX_BUFFER0); 
    if (numTimeSteps >= 2) rtcUnmapBuffer(scene,geomID,RTC_VERTEX_BUFFER1); 
    rtcUnmapBuffer(scene,geomID,RTC_INDEX_BUFFER);
    return geomID;
  }

  unsigned addGarbageTriangles (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, size_t numTriangles, bool motion)
  {
    /* create a triangulated sphere */
    size_t numTimeSteps = motion ? 2 : 1;
    unsigned mesh = rtcNewTriangleMesh (scene, flag, numTriangles, 3*numTriangles,numTimeSteps);
    
    /* map triangle and vertex buffer */
    if (numTimeSteps >= 1) {
      int* v = (int*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER0); 
      for (size_t i=0; i<4*3*numTriangles; i++) v[i] = random<uint32_t>();
      rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER0); 
    }
    if (numTimeSteps >= 2) {
      int* v = (int*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER1); 
      for (size_t i=0; i<4*3*numTriangles; i++) v[i] = random<uint32_t>();
      rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER1); 
    }
    
    Triangle* triangles = (Triangle*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    for (size_t i=0; i<numTriangles; i++) {
      triangles[i].v0 = (random<int>() % 32 == 0) ? random<uint32_t>() : 3*i+0;
      triangles[i].v1 = (random<int>() % 32 == 0) ? random<uint32_t>() : 3*i+1;
      triangles[i].v2 = (random<int>() % 32 == 0) ? random<uint32_t>() : 3*i+2;
    }
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);

    return mesh;
  }

  unsigned addGarbageHair (RTCDevice g_device, const RTCSceneRef& scene, RTCGeometryFlags flag, size_t numCurves, bool motion)
  {
    /* create a triangulated sphere */
    size_t numTimeSteps = motion ? 2 : 1;
    unsigned mesh = rtcNewHairGeometry (scene, flag, numCurves, 4*numCurves,numTimeSteps);
    
    /* map triangle and vertex buffer */
    if (numTimeSteps >= 1) {
      int* v = (int*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER0); 
      for (size_t i=0; i<4*4*numCurves; i++) v[i] = random<uint32_t>();
      rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER0); 
    }
    if (numTimeSteps >= 2) {
      int* v = (int*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER1); 
      for (size_t i=0; i<4*4*numCurves; i++) v[i] = random<uint32_t>();
      rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER1); 
    }
    
    int* curves = (int*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
    for (size_t i=0; i<numCurves; i++) 
      curves[i] = (random<int>() % 32 == 0) ? random<uint32_t>() : 4*i;
    rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);

    return mesh;
  }

  struct Sphere
  {
    ALIGNED_CLASS;
  public:
    Sphere () : pos(zero), r(zero) {}
    Sphere (const Vec3fa& pos, float r) : pos(pos), r(r) {}
    __forceinline BBox3fa bounds() const { return BBox3fa(pos-Vec3fa(r),pos+Vec3fa(r)); }
  public:
    Vec3fa pos;
    float r;
  };

  void BoundsFunc(Sphere* sphere, size_t index, BBox3fa* bounds_o)
  {
    bounds_o->lower.x = sphere->pos.x-sphere->r;
    bounds_o->lower.y = sphere->pos.y-sphere->r;
    bounds_o->lower.z = sphere->pos.z-sphere->r;
    bounds_o->upper.x = sphere->pos.x+sphere->r;
    bounds_o->upper.y = sphere->pos.y+sphere->r;
    bounds_o->upper.z = sphere->pos.z+sphere->r;
  }

  void IntersectFunc(void* ptr, RTCRay& ray, size_t item) {
  }

  void IntersectFunc4(const void* valid, void* ptr, RTCRay4& ray, size_t item) {
  }

  void IntersectFunc8(const void* valid, void* ptr, RTCRay8& ray, size_t item) {
  }

  void IntersectFunc16(const void* valid, void* ptr, RTCRay16& ray, size_t item) {
  }

  void OccludedFunc (void* ptr, RTCRay& ray, size_t item) {
  }

  void OccludedFunc4 (const void* valid, void* ptr, RTCRay4& ray, size_t item) {
  }

  void OccludedFunc8 (const void* valid, void* ptr, RTCRay8& ray, size_t item) {
  }

  void OccludedFunc16 (const void* valid, void* ptr, RTCRay16& ray, size_t item) {
  }

  unsigned addUserGeometryEmpty (RTCDevice g_device, const RTCSceneRef& scene, Sphere* sphere)
  {
    BBox3fa bounds = sphere->bounds(); 
    unsigned geom = rtcNewUserGeometry (scene,1);
    rtcSetBoundsFunction(scene,geom,(RTCBoundsFunc)BoundsFunc);
    rtcSetUserData(scene,geom,sphere);
    rtcSetIntersectFunction(scene,geom,IntersectFunc);
#if defined(RTCORE_RAY_PACKETS)
    rtcSetIntersectFunction4(scene,geom,IntersectFunc4);
    rtcSetIntersectFunction8(scene,geom,IntersectFunc8);
    rtcSetIntersectFunction16(scene,geom,&IntersectFunc16);
#endif
    rtcSetOccludedFunction(scene,geom,OccludedFunc);
#if defined(RTCORE_RAY_PACKETS)
    rtcSetOccludedFunction4(scene,geom,OccludedFunc4);
    rtcSetOccludedFunction8(scene,geom,OccludedFunc8);
    rtcSetOccludedFunction16(scene,geom,&OccludedFunc16);
#endif
    return geom;
  }


  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////


  struct MultipleDevicesTest : public VerifyApplication::Test
  {
    MultipleDevicesTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}

    bool run(VerifyApplication* state)
    {
      /* test creation of multiple devices */
      RTCDevice device1 = rtcNewDevice("threads=4");
      AssertNoError(device1);
      RTCDevice device2 = rtcNewDevice("threads=8");
      AssertNoError(device2);
      RTCDevice device3 = rtcNewDevice("threads=12");
      AssertNoError(device3);
      rtcDeleteDevice(device1);
      rtcDeleteDevice(device3);
      rtcDeleteDevice(device2);
      return true;
    }
  };

  struct EmptySceneTest : public VerifyApplication::Test
  {
    EmptySceneTest (std::string name, RTCSceneFlags sflags)
      : VerifyApplication::Test(name,VerifyApplication::PASS), sflags(sflags) {}

    bool run(VerifyApplication* state)
    {
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      AssertNoError(state->device);
      rtcCommit (scene);
      AssertNoError(state->device);
      return true;
    }

  public:
    RTCSceneFlags sflags;
  };

  struct BaryDistanceTest : public VerifyApplication::Test
  {
    BaryDistanceTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}

    bool run(VerifyApplication* state)
    {
      std::vector<Vertex> m_vertices;
      std::vector<Triangle> m_triangles;
      
      const float length = 1000.0f;
      const float width = 1000.0f;
      
      m_vertices.resize(4);
      m_vertices[0] = Vertex(-length / 2.0f, -width / 2.0f, 0);
      m_vertices[1] = Vertex( length / 2.0f, -width / 2.0f, 0);
      m_vertices[2] = Vertex( length / 2.0f,  width / 2.0f, 0);
      m_vertices[3] = Vertex(-length / 2.0f,  width / 2.0f, 0);
      
      m_triangles.resize(2);
      m_triangles[0] = Triangle(0, 1, 2);
      m_triangles[1] = Triangle(2, 3, 0);
      
      //const RTCSceneFlags flags = RTCSceneFlags(0); 
      const RTCSceneFlags flags = RTC_SCENE_ROBUST;
      const RTCSceneRef mainSceneId = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | flags , RTC_INTERSECT1);
      
      const unsigned int id = rtcNewTriangleMesh(mainSceneId, RTC_GEOMETRY_STATIC, m_triangles.size(), m_vertices.size());
      
      rtcSetBuffer(mainSceneId, id, RTC_VERTEX_BUFFER, m_vertices.data(), 0, sizeof(Vertex));
      rtcSetBuffer(mainSceneId, id, RTC_INDEX_BUFFER, m_triangles.data(), 0, sizeof(Triangle));
      
      rtcCommit(mainSceneId);
      
      RTCRay ray;
      ray.org[0] = 0.1f;
      ray.org[1] = 1.09482f;
      ray.org[2] = 29.8984f;
      ray.dir[0] = 0.f;
      ray.dir[1] = 0.99482f;
      ray.dir[2] = -0.101655f;
      ray.tnear = 0.05f;
      ray.tfar  = inf;
      ray.mask  = -1;
      
      ray.geomID = RTC_INVALID_GEOMETRY_ID;
      ray.primID = RTC_INVALID_GEOMETRY_ID;
      ray.instID = RTC_INVALID_GEOMETRY_ID;
      
      rtcIntersect(mainSceneId, ray);
      
      if (ray.geomID == RTC_INVALID_GEOMETRY_ID) 
        throw std::runtime_error("no triangle hit");
      
      const Triangle &triangle = m_triangles[ray.primID];
      
      const Vertex &v0_ = m_vertices[triangle.v0];
      const Vertex &v1_ = m_vertices[triangle.v1];
      const Vertex &v2_ = m_vertices[triangle.v2];
      
      const Vec3fa v0(v0_.x, v0_.y, v0_.z);
      const Vec3fa v1(v1_.x, v1_.y, v1_.z);
      const Vec3fa v2(v2_.x, v2_.y, v2_.z);
      
      const Vec3fa hit_tri = v0 + ray.u * (v1 - v0) + ray.v * (v2 - v0);
      
      const Vec3fa ray_org = Vec3fa(ray.org[0], ray.org[1], ray.org[2]);
      const Vec3fa ray_dir = Vec3fa(ray.dir[0], ray.dir[1], ray.dir[2]);
      
      const Vec3fa hit_tfar = ray_org + ray.tfar * ray_dir;
      const Vec3fa delta = hit_tri - hit_tfar;
      const float distance = embree::length(delta);
      
      return distance < 0.0002f;
    }
  };

  struct FlagsTest : public VerifyApplication::Test
  {
    FlagsTest (std::string name, VerifyApplication::TestType type, RTCSceneFlags sceneFlags, RTCGeometryFlags geomFlags)
      : VerifyApplication::Test(name,type), sceneFlags(sceneFlags), geomFlags(geomFlags) {}

    bool run(VerifyApplication* state)
    {
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sceneFlags,aflags);
      AssertNoError(state->device);
      rtcNewTriangleMesh (scene, geomFlags, 0, 0);
      AssertNoError(state->device);
      rtcNewHairGeometry (scene, geomFlags, 0, 0);
      AssertNoError(state->device);
      rtcCommit (scene);
      AssertNoError(state->device);
      scene = nullptr;
      return true;
    }

  public:
    RTCSceneFlags sceneFlags;
    RTCGeometryFlags geomFlags;
  };
  
  struct StaticSceneTest : public VerifyApplication::Test
  {
    StaticSceneTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}

    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,aflags);
      AssertNoError(state->device);
      unsigned geom0 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,zero,1.0f,50);
      AssertNoError(state->device);
      unsigned geom1 = addSubdivSphere(state->device,scene,RTC_GEOMETRY_STATIC,zero,1.0f,10,16);
      AssertNoError(state->device);
      unsigned geom2 = addHair(state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(0,0,0),1.0f,0.5f,100);
      AssertNoError(state->device);
      rtcCommit (scene);
      AssertNoError(state->device);
      rtcDisable(scene,geom0); // static scene cannot get modified anymore after commit
      AssertAnyError(state->device);
      scene = nullptr;
      return true;
    }
  };

  struct UnmappedBeforeCommitTest : public VerifyApplication::Test
  {
    UnmappedBeforeCommitTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}

    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,aflags);
      AssertNoError(state->device);
      unsigned geom0 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,zero,1.0f,50);
      unsigned geom1 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,zero,1.0f,50);
      AssertNoError(state->device);
      rtcMapBuffer(scene,geom0,RTC_INDEX_BUFFER);
      rtcMapBuffer(scene,geom0,RTC_VERTEX_BUFFER);
      AssertNoError(state->device);
      rtcCommit (scene);
      AssertError(state->device,RTC_INVALID_OPERATION); // error, buffers still mapped
      scene = nullptr;
      return true;
    }
  };

  struct GetBoundsTest : public VerifyApplication::Test
  {
    GetBoundsTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}

    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,RTC_INTERSECT1);
      AssertNoError(state->device);
      BBox3fa bounds0;
      unsigned geom0 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,zero,1.0f,50,-1,0,&bounds0);
      AssertNoError(state->device);
      rtcCommit (scene);
      AssertNoError(state->device);
      BBox3fa bounds1;
      rtcGetBounds(scene,(RTCBounds&)bounds1);
      scene = nullptr;
      return bounds0 == bounds1;
    }
  };

  struct BufferStrideTest : public VerifyApplication::Test
  {
    BufferStrideTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,aflags);
      AssertNoError(state->device);
      unsigned geom = rtcNewTriangleMesh (scene, RTC_GEOMETRY_STATIC, 16, 16);
      AssertNoError(state->device);
      avector<char> indexBuffer(8+16*6*sizeof(int));
      avector<char> vertexBuffer(12+16*9*sizeof(float)+4);
      
      rtcSetBuffer(scene,geom,RTC_INDEX_BUFFER,indexBuffer.data(),1,3*sizeof(int));
      AssertError(state->device,RTC_INVALID_OPERATION);
      rtcSetBuffer(scene,geom,RTC_VERTEX_BUFFER,vertexBuffer.data(),1,3*sizeof(float));
      AssertError(state->device,RTC_INVALID_OPERATION);

      rtcSetBuffer(scene,geom,RTC_INDEX_BUFFER,indexBuffer.data(),0,3*sizeof(int)+3);
      AssertError(state->device,RTC_INVALID_OPERATION);
      rtcSetBuffer(scene,geom,RTC_VERTEX_BUFFER,vertexBuffer.data(),0,3*sizeof(float)+3);
      AssertError(state->device,RTC_INVALID_OPERATION);
      
      rtcSetBuffer(scene,geom,RTC_INDEX_BUFFER,indexBuffer.data(),0,3*sizeof(int));
      AssertNoError(state->device);
      rtcSetBuffer(scene,geom,RTC_VERTEX_BUFFER,vertexBuffer.data(),0,3*sizeof(float));
      AssertNoError(state->device);
      
      rtcSetBuffer(scene,geom,RTC_INDEX_BUFFER,indexBuffer.data(),8,6*sizeof(int));
      AssertNoError(state->device);
      rtcSetBuffer(scene,geom,RTC_VERTEX_BUFFER,vertexBuffer.data(),12,9*sizeof(float));
      AssertNoError(state->device);
      
      rtcSetBuffer(scene,geom,RTC_INDEX_BUFFER,indexBuffer.data(),0,3*sizeof(int));
      AssertNoError(state->device);
      
      rtcSetBuffer(scene,geom,RTC_VERTEX_BUFFER,vertexBuffer.data(),0,4*sizeof(float));
      AssertNoError(state->device);
      
      scene = nullptr;
      return true;
    }
  };
  
  struct DynamicEnableDisableTest : public VerifyApplication::Test
  {
    DynamicEnableDisableTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_DYNAMIC,aflags);
      AssertNoError(state->device);
      unsigned geom0 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(-1,0,-1),1.0f,50);
      //unsigned geom1 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(-1,0,+1),1.0f,50);
      unsigned geom1 = addHair  (state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(-1,0,+1),1.0f,1.0f,1);
      unsigned geom2 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(+1,0,-1),1.0f,50);
      //unsigned geom3 = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(+1,0,+1),1.0f,50);
      unsigned geom3 = addHair  (state->device,scene,RTC_GEOMETRY_STATIC,Vec3fa(+1,0,+1),1.0f,1.0f,1);
      AssertNoError(state->device);
      
      for (size_t i=0; i<16; i++) 
      {
        bool enabled0 = i & 1, enabled1 = i & 2, enabled2 = i & 4, enabled3 = i & 8;
        if (enabled0) rtcEnable(scene,geom0); else rtcDisable(scene,geom0); AssertNoError(state->device);
        if (enabled1) rtcEnable(scene,geom1); else rtcDisable(scene,geom1); AssertNoError(state->device);
        if (enabled2) rtcEnable(scene,geom2); else rtcDisable(scene,geom2); AssertNoError(state->device);
        if (enabled3) rtcEnable(scene,geom3); else rtcDisable(scene,geom3); AssertNoError(state->device);
        rtcCommit (scene);
        AssertNoError(state->device);
        {
          RTCRay ray0 = makeRay(Vec3fa(-1,10,-1),Vec3fa(0,-1,0));
          RTCRay ray1 = makeRay(Vec3fa(-1,10,+1),Vec3fa(0,-1,0)); 
          RTCRay ray2 = makeRay(Vec3fa(+1,10,-1),Vec3fa(0,-1,0)); 
          RTCRay ray3 = makeRay(Vec3fa(+1,10,+1),Vec3fa(0,-1,0)); 
          rtcIntersect(scene,ray0);
          rtcIntersect(scene,ray1);
          rtcIntersect(scene,ray2);
          rtcIntersect(scene,ray3);
          bool ok0 = enabled0 ? ray0.geomID == 0 : ray0.geomID == -1;
          bool ok1 = enabled1 ? ray1.geomID == 1 : ray1.geomID == -1;
          bool ok2 = enabled2 ? ray2.geomID == 2 : ray2.geomID == -1;
          bool ok3 = enabled3 ? ray3.geomID == 3 : ray3.geomID == -1;
          if (!ok0 || !ok1 || !ok2 || !ok3) return false;
        }
      }
      scene = nullptr;
      return true;
    }
  };

  struct GetUserDataTest : public VerifyApplication::Test
  {
    GetUserDataTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool run(VerifyApplication* state)
    {
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,RTC_INTERSECT1);
      AssertNoError(state->device);
      unsigned geom0 = rtcNewTriangleMesh (scene, RTC_GEOMETRY_STATIC, 0, 0, 1);
      AssertNoError(state->device);
      rtcSetUserData(scene,geom0,(void*)1);
      
      unsigned geom1 = rtcNewSubdivisionMesh(scene, RTC_GEOMETRY_STATIC, 0, 0, 0, 0, 0, 0, 1);
      AssertNoError(state->device);
      rtcSetUserData(scene,geom1,(void*)2);
      
      unsigned geom2 = rtcNewHairGeometry (scene, RTC_GEOMETRY_STATIC, 0, 0, 1);
      AssertNoError(state->device);
      rtcSetUserData(scene,geom2,(void*)3);
      
      unsigned geom3 = rtcNewUserGeometry (scene,0);
      AssertNoError(state->device);
      rtcSetUserData(scene,geom3,(void*)4);
      
      rtcCommit (scene);
      AssertNoError(state->device);
      
      if ((size_t)rtcGetUserData(scene,geom0) != 1) return false;
      if ((size_t)rtcGetUserData(scene,geom1) != 2) return false;
      if ((size_t)rtcGetUserData(scene,geom2) != 3) return false;
      if ((size_t)rtcGetUserData(scene,geom3) != 4) return false;
      
      scene = nullptr;
      AssertNoError(state->device);
      return true;
    }
  };

  void move_mesh_vec3f(const RTCSceneRef& scene, unsigned mesh, size_t numVertices, Vec3fa& pos) 
  {
    Vertex3f* vertices = (Vertex3f*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    for (size_t i=0; i<numVertices; i++) vertices[i] += Vertex3f(pos);
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER);
    rtcUpdate(scene,mesh);
  }

  void move_mesh_vec3fa(const RTCSceneRef& scene, unsigned mesh, size_t numVertices, Vec3fa& pos) 
  {
    Vertex3fa* vertices = (Vertex3fa*) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
    for (size_t i=0; i<numVertices; i++) vertices[i] += Vertex3fa(pos);
    rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER);
    rtcUpdate(scene,mesh);
  }
  
  struct UpdateTest : public VerifyApplication::Test
  {
    UpdateTest (std::string name, RTCGeometryFlags flags)
      : VerifyApplication::Test(name,VerifyApplication::PASS), flags(flags) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_DYNAMIC,aflags);
      AssertNoError(state->device);
      size_t numPhi = 50;
      size_t numVertices = 2*numPhi*(numPhi+1);
      Vec3fa pos0 = Vec3fa(-10,0,-10);
      Vec3fa pos1 = Vec3fa(-10,0,+10);
      Vec3fa pos2 = Vec3fa(+10,0,-10);
      Vec3fa pos3 = Vec3fa(+10,0,+10);
      unsigned geom0 = addSphere(state->device,scene,flags,pos0,1.0f,numPhi);
      unsigned geom1 = addHair  (state->device,scene,flags,pos1,1.0f,1.0f,1);
      unsigned geom2 = addSphere(state->device,scene,flags,pos2,1.0f,numPhi);
      unsigned geom3 = addHair  (state->device,scene,flags,pos3,1.0f,1.0f,1);
      AssertNoError(state->device);
      
      for (size_t i=0; i<16; i++) 
      {
        bool move0 = i & 1, move1 = i & 2, move2 = i & 4, move3 = i & 8;
        Vec3fa ds(2,0.1f,2);
        if (move0) { move_mesh_vec3f (scene,geom0,numVertices,ds); pos0 += ds; }
        if (move1) { move_mesh_vec3fa(scene,geom1,4,ds); pos1 += ds; }
        if (move2) { move_mesh_vec3f (scene,geom2,numVertices,ds); pos2 += ds; }
        if (move3) { move_mesh_vec3fa(scene,geom3,4,ds); pos3 += ds; }
        rtcCommit (scene);
        AssertNoError(state->device);
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); 
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); 
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); 
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); 
          rtcIntersect(scene,ray0);
          rtcIntersect(scene,ray1);
          rtcIntersect(scene,ray2);
          rtcIntersect(scene,ray3);
          if (ray0.geomID != 0 || 
              ray1.geomID != 1 || 
              ray2.geomID != 2 || 
              ray3.geomID != 3) return false;
          
#if HAS_INTERSECT4
          RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
          setRay(ray4,0,ray0);
          setRay(ray4,1,ray1);
          setRay(ray4,2,ray2);
          setRay(ray4,3,ray3);
          __aligned(16) int valid4[4] = { -1,-1,-1,-1 };
          rtcIntersect4(valid4,scene,ray4);
          if (ray4.geomID[0] != 0 || 
              ray4.geomID[1] != 1 || 
              ray4.geomID[2] != 2 || 
              ray4.geomID[3] != 3) return false;
#endif
          
#if HAS_INTERSECT8
          if (hasISA(AVX)) 
          {
            RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
            setRay(ray8,0,ray0);
            setRay(ray8,1,ray1);
            setRay(ray8,2,ray2);
            setRay(ray8,3,ray3);
            __aligned(32) int valid8[8] = { -1,-1,-1,-1, 0, 0, 0, 0 };
            rtcIntersect8(valid8,scene,ray8);
            if (ray8.geomID[0] != 0 || 
                ray8.geomID[1] != 1 || 
                ray8.geomID[2] != 2 || 
                ray8.geomID[3] != 3) return false;
          }
#endif
          
#if HAS_INTERSECT16
          if (hasISA(AVX512KNL) || hasISA(KNC)) 
          {
            RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
            setRay(ray16,0,ray0);
            setRay(ray16,1,ray1);
            setRay(ray16,2,ray2);
            setRay(ray16,3,ray3);
            __aligned(64) int valid16[16] = { -1,-1,-1,-1,+0,+0,+0,+0, 
                                              +0,+0,+0,+0,+0,+0,+0,+0 };
            rtcIntersect16(valid16,scene,ray16);
            if (ray16.geomID[0] != 0 || 
                ray16.geomID[1] != 1 || 
                ray16.geomID[2] != 2 || 
                ray16.geomID[3] != 3) return false;
          }
#endif
        }
      }
      scene = nullptr;
      return true;
    }
    
  public:
    RTCGeometryFlags flags;
  };
  
  struct RayMasksTest : public VerifyApplication::Test
  {
    RayMasksTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool rtcore_ray_masks_intersect(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags)
    {
      ClearBuffers clear_before_return;
      bool passed = true;
      Vec3fa pos0 = Vec3fa(-10,0,-10);
      Vec3fa pos1 = Vec3fa(-10,0,+10);
      Vec3fa pos2 = Vec3fa(+10,0,-10);
      Vec3fa pos3 = Vec3fa(+10,0,+10);
      
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      unsigned geom0 = addSphere(state->device,scene,gflags,pos0,1.0f,50);
      //unsigned geom1 = addSphere(state->device,scene,gflags,pos1,1.0f,50);
      unsigned geom1 = addHair  (state->device,scene,gflags,pos1,1.0f,1.0f,1);
      unsigned geom2 = addSphere(state->device,scene,gflags,pos2,1.0f,50);
      //unsigned geom3 = addSphere(state->device,scene,gflags,pos3,1.0f,50);
      unsigned geom3 = addHair  (state->device,scene,gflags,pos3,1.0f,1.0f,1);
      rtcSetMask(scene,geom0,1);
      rtcSetMask(scene,geom1,2);
      rtcSetMask(scene,geom2,4);
      rtcSetMask(scene,geom3,8);
      rtcCommit (scene);
      
      for (size_t i=0; i<16; i++) 
      {
        int mask0 = i;
        int mask1 = i+1;
        int mask2 = i+2;
        int mask3 = i+3;
        
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          rtcIntersect(scene,ray0);
          rtcIntersect(scene,ray1);
          rtcIntersect(scene,ray2);
          rtcIntersect(scene,ray3);
          bool ok0 = mask0 & 1 ? ray0.geomID == 0 : ray0.geomID == -1;
          bool ok1 = mask1 & 2 ? ray1.geomID == 1 : ray1.geomID == -1;
          bool ok2 = mask2 & 4 ? ray2.geomID == 2 : ray2.geomID == -1;
          bool ok3 = mask3 & 8 ? ray3.geomID == 3 : ray3.geomID == -1;
          if (!ok0 || !ok1 || !ok2 || !ok3) passed = false;
        }
        
#if HAS_INTERSECT4
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
          setRay(ray4,0,ray0);
          setRay(ray4,1,ray1);
          setRay(ray4,2,ray2);
          setRay(ray4,3,ray3);
          __aligned(16) int valid4[4] = { -1,-1,-1,-1 };
          rtcIntersect4(valid4,scene,ray4);
          bool ok4a = mask0 & 1 ? ray4.geomID[0] == 0 : ray4.geomID[0] == -1;
          bool ok4b = mask1 & 2 ? ray4.geomID[1] == 1 : ray4.geomID[1] == -1;
          bool ok4c = mask2 & 4 ? ray4.geomID[2] == 2 : ray4.geomID[2] == -1;
          bool ok4d = mask3 & 8 ? ray4.geomID[3] == 3 : ray4.geomID[3] == -1;
          if (!ok4a || !ok4b || !ok4c || !ok4d) passed = false; 
        }
        
#if HAS_INTERSECT8
        if (hasISA(AVX))
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
          setRay(ray8,0,ray0);
          setRay(ray8,1,ray1);
          setRay(ray8,2,ray2);
          setRay(ray8,3,ray3);
          __aligned(32) int valid8[8] = { -1,-1,-1,-1,0,0,0,0 };
          rtcIntersect8(valid8,scene,ray8);
          bool ok8a = mask0 & 1 ? ray8.geomID[0] == 0 : ray8.geomID[0] == -1;
          bool ok8b = mask1 & 2 ? ray8.geomID[1] == 1 : ray8.geomID[1] == -1;
          bool ok8c = mask2 & 4 ? ray8.geomID[2] == 2 : ray8.geomID[2] == -1;
          bool ok8d = mask3 & 8 ? ray8.geomID[3] == 3 : ray8.geomID[3] == -1;
          if (!ok8a || !ok8b || !ok8c || !ok8d) passed = false; 
        }
#endif
        
#endif
        
#if HAS_INTERSECT16
        if (hasISA(AVX512KNL) || hasISA(KNC))
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
          setRay(ray16,0,ray0);
          setRay(ray16,1,ray1);
          setRay(ray16,2,ray2);
          setRay(ray16,3,ray3);
          __aligned(64) int valid16[16] = { -1,-1,-1,-1,0,0,0,0,0,0,0,0,0,0,0,0 };
          rtcIntersect16(valid16,scene,ray16);
          bool ok16a = mask0 & 1 ? ray16.geomID[0] == 0 : ray16.geomID[0] == -1;
          bool ok16b = mask1 & 2 ? ray16.geomID[1] == 1 : ray16.geomID[1] == -1;
          bool ok16c = mask2 & 4 ? ray16.geomID[2] == 2 : ray16.geomID[2] == -1;
          bool ok16d = mask3 & 8 ? ray16.geomID[3] == 3 : ray16.geomID[3] == -1;
          if (!ok16a || !ok16b || !ok16c || !ok16d) passed = false;
        }
#endif
        
        
      }
      scene = nullptr;
      return passed;
    }
    
    bool rtcore_ray_masks_occluded(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags)
    {
      ClearBuffers clear_before_return;
      bool passed = true;
      Vec3fa pos0 = Vec3fa(-10,0,-10);
      Vec3fa pos1 = Vec3fa(-10,0,+10);
      Vec3fa pos2 = Vec3fa(+10,0,-10);
      Vec3fa pos3 = Vec3fa(+10,0,+10);
      
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      unsigned geom0 = addSphere(state->device,scene,gflags,pos0,1.0f,50);
      unsigned geom1 = addSphere(state->device,scene,gflags,pos1,1.0f,50);
      unsigned geom2 = addSphere(state->device,scene,gflags,pos2,1.0f,50);
      unsigned geom3 = addSphere(state->device,scene,gflags,pos3,1.0f,50);
      rtcSetMask(scene,geom0,1);
      rtcSetMask(scene,geom1,2);
      rtcSetMask(scene,geom2,4);
      rtcSetMask(scene,geom3,8);
      rtcCommit (scene);
      
      for (size_t i=0; i<16; i++) 
      {
        int mask0 = i;
        int mask1 = i+1;
        int mask2 = i+2;
        int mask3 = i+3;
        
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          rtcOccluded(scene,ray0);
          rtcOccluded(scene,ray1);
          rtcOccluded(scene,ray2);
          rtcOccluded(scene,ray3);
          bool ok0 = mask0 & 1 ? ray0.geomID == 0 : ray0.geomID == -1;
          bool ok1 = mask1 & 2 ? ray1.geomID == 0 : ray1.geomID == -1;
          bool ok2 = mask2 & 4 ? ray2.geomID == 0 : ray2.geomID == -1;
          bool ok3 = mask3 & 8 ? ray3.geomID == 0 : ray3.geomID == -1;
          
          if (!ok0 || !ok1 || !ok2 || !ok3) passed = false;
        }
        
#if HAS_INTERSECT4
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
          setRay(ray4,0,ray0);
          setRay(ray4,1,ray1);
          setRay(ray4,2,ray2);
          setRay(ray4,3,ray3);
          __aligned(16) int valid4[4] = { -1,-1,-1,-1 };
          rtcOccluded4(valid4,scene,ray4);
          bool ok4a = mask0 & 1 ? ray4.geomID[0] == 0 : ray4.geomID[0] == -1;
          bool ok4b = mask1 & 2 ? ray4.geomID[1] == 0 : ray4.geomID[1] == -1;
          bool ok4c = mask2 & 4 ? ray4.geomID[2] == 0 : ray4.geomID[2] == -1;
          bool ok4d = mask3 & 8 ? ray4.geomID[3] == 0 : ray4.geomID[3] == -1;
          if (!ok4a || !ok4b || !ok4c || !ok4d) passed = false;
        }
        
#if HAS_INTERSECT8
        if (hasISA(AVX)) 
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
          setRay(ray8,0,ray0);
          setRay(ray8,1,ray1);
          setRay(ray8,2,ray2);
          setRay(ray8,3,ray3);
          __aligned(32) int valid8[8] = { -1,-1,-1,-1,0,0,0,0 };
          rtcOccluded8(valid8,scene,ray8);
          bool ok8a = mask0 & 1 ? ray8.geomID[0] == 0 : ray8.geomID[0] == -1;
          bool ok8b = mask1 & 2 ? ray8.geomID[1] == 0 : ray8.geomID[1] == -1;
          bool ok8c = mask2 & 4 ? ray8.geomID[2] == 0 : ray8.geomID[2] == -1;
          bool ok8d = mask3 & 8 ? ray8.geomID[3] == 0 : ray8.geomID[3] == -1;
          if (!ok8a || !ok8b || !ok8c || !ok8d) passed = false;
        }
#endif
        
#endif
        
#if HAS_INTERSECT16
        if (hasISA(AVX512KNL) || hasISA(KNC))
        {
          RTCRay ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.mask = mask0;
          RTCRay ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.mask = mask1;
          RTCRay ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.mask = mask2;
          RTCRay ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.mask = mask3;
          
          RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
          setRay(ray16,0,ray0);
          setRay(ray16,1,ray1);
          setRay(ray16,2,ray2);
          setRay(ray16,3,ray3);
          __aligned(64) int valid16[16] = { -1,-1,-1,-1,0,0,0,0,0,0,0,0,0,0,0,0 };
          
          rtcOccluded16(valid16,scene,ray16);
          
          bool ok16a = mask0 & 1 ? ray16.geomID[0] == 0 : ray16.geomID[0] == -1;
          bool ok16b = mask1 & 2 ? ray16.geomID[1] == 0 : ray16.geomID[1] == -1;
          bool ok16c = mask2 & 4 ? ray16.geomID[2] == 0 : ray16.geomID[2] == -1;
          bool ok16d = mask3 & 8 ? ray16.geomID[3] == 0 : ray16.geomID[3] == -1;
          if (!ok16a || !ok16b || !ok16c || !ok16d) passed = false;
        }
        
#endif
      }
      scene = nullptr;
      return passed;
    }
    
    bool run(VerifyApplication* state)
    {
      bool passed = true;
      for (int i=0; i<numSceneFlags; i++) 
      {
        RTCSceneFlags flag = getSceneFlag(i);
        bool ok0 = rtcore_ray_masks_intersect(state,flag,RTC_GEOMETRY_STATIC);
        if (ok0) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok0;
        bool ok1 = rtcore_ray_masks_occluded(state,flag,RTC_GEOMETRY_STATIC);
        if (ok1) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok1;
        fflush(stdout);
      }
      return passed;
    }
  };

  struct BuildTest : public VerifyApplication::Test
  {
    BuildTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool rtcore_build(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      addSphere(state->device,scene,gflags,zero,1E-24f,50);
      addHair(state->device,scene,gflags,zero,1E-24f,1E-26f,100,1E-26f);
      addSphere(state->device,scene,gflags,zero,1E-24f,50);
      addHair(state->device,scene,gflags,zero,1E-24f,1E-26f,100,1E-26f);
      rtcCommit (scene);
      scene = nullptr;
      return true;
    }
  
    bool run(VerifyApplication* state)
    {
      bool passed = true;
      for (int i=0; i<numSceneGeomFlags; i++) 
      {
        RTCSceneFlags sflags; RTCGeometryFlags gflags;
        getSceneGeomFlag(i,sflags,gflags);
        bool ok = rtcore_build(state,sflags,gflags);
        if (ok) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok;
        fflush(stdout);
      }
      return passed;
    }
  };

  struct IntersectionFilterTest : public VerifyApplication::Test
  {
    bool subdiv;

    IntersectionFilterTest (std::string name, bool subdiv)
      : VerifyApplication::Test(name,VerifyApplication::PASS), subdiv(subdiv) {}
    
    static void intersectionFilter1(void* ptr, RTCRay& ray) 
    {
      if ((size_t)ptr != 123) 
        return;
      
      if (ray.primID & 2)
        ray.geomID = -1;
    }
    
    static void intersectionFilter4(const void* valid_i, void* ptr, RTCRay4& ray) 
    {
      if ((size_t)ptr != 123) 
        return;
      
      int* valid = (int*)valid_i;
      for (size_t i=0; i<4; i++)
        if (valid[i] == -1)
          if (ray.primID[i] & 2) 
            ray.geomID[i] = -1;
    }
    
    static void intersectionFilter8(const void* valid_i, void* ptr, RTCRay8& ray) 
    {
      if ((size_t)ptr != 123) 
        return;
      
      int* valid = (int*)valid_i;
      for (size_t i=0; i<8; i++)
        if (valid[i] == -1)
          if (ray.primID[i] & 2) 
            ray.geomID[i] = -1;
    }
    
    static void intersectionFilter16(const void* valid_i, void* ptr, RTCRay16& ray) 
    {
      if ((size_t)ptr != 123) 
        return;
      
      unsigned int valid = *(unsigned int*)valid_i;
      for (size_t i=0; i<16; i++)
	if (valid & ((unsigned int)1 << i))
	  if (ray.primID[i] & 2) 
	    ray.geomID[i] = -1;
    }
    
    bool rtcore_filter_intersect(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags, bool subdiv)
    {
      ClearBuffers clear_before_return;
      bool passed = true;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      Vec3fa p0(-0.75f,-0.25f,-10.0f), dx(4,0,0), dy(0,4,0);
      int geom0 = 0;
      if (subdiv) geom0 = addSubdivPlane (state->device,scene, gflags, 4, p0, dx, dy);
      else        geom0 = addPlane (state->device,scene, gflags, 4, p0, dx, dy);
      rtcSetUserData(scene,geom0,(void*)123);
      rtcSetIntersectionFilterFunction(scene,geom0,intersectionFilter1);
#if defined(RTCORE_RAY_PACKETS)
      rtcSetIntersectionFilterFunction4(scene,geom0,intersectionFilter4);
      rtcSetIntersectionFilterFunction8(scene,geom0,intersectionFilter8);
      rtcSetIntersectionFilterFunction16(scene,geom0,intersectionFilter16);
#endif
      rtcCommit (scene);
      
      for (size_t iy=0; iy<4; iy++) 
      {
        for (size_t ix=0; ix<4; ix++) 
        {
          int primID = iy*4+ix;
          if (!subdiv) primID *= 2;
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            rtcIntersect(scene,ray0);
            bool ok0 = (primID & 2) ? (ray0.geomID == -1) : (ray0.geomID == 0);
            if (!ok0) passed = false;
          }
          
#if HAS_INTERSECT4
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
            setRay(ray4,0,ray0);
            __aligned(16) int valid4[4] = { -1,0,0,0 };
            rtcIntersect4(valid4,scene,ray4);
            bool ok0 = (primID & 2) ? (ray4.geomID[0] == -1) : (ray4.geomID[0] == 0);
            if (!ok0) passed = false;
          }
          
#if HAS_INTERSECT8
          if (hasISA(AVX))
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
            setRay(ray8,0,ray0);
            __aligned(32) int valid8[8] = { -1,0,0,0,0,0,0,0 };
            rtcIntersect8(valid8,scene,ray8);
            bool ok0 = (primID & 2) ? (ray8.geomID[0] == -1) : (ray8.geomID[0] == 0);
            if (!ok0) passed = false;
          }
#endif
          
#endif
          
#if HAS_INTERSECT16
          if (hasISA(AVX512KNL) || hasISA(KNC))
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
            setRay(ray16,0,ray0);
            __aligned(64) int valid16[16] = { -1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
            rtcIntersect16(valid16,scene,ray16);
            bool ok0 = (primID & 2) ? (ray16.geomID[0] == -1) : (ray16.geomID[0] == 0);
            if (!ok0) passed = false;
            
          }
#endif
        }
      }
      scene = nullptr;
      return passed;
    }
    
    bool rtcore_filter_occluded(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags, bool subdiv)
    {
      ClearBuffers clear_before_return;
      bool passed = true;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      Vec3fa p0(-0.75f,-0.25f,-10.0f), dx(4,0,0), dy(0,4,0);
      int geom0 = 0;
      if (subdiv) geom0 = addSubdivPlane (state->device, scene, gflags, 4, p0, dx, dy);
      else        geom0 = addPlane (state->device, scene, gflags, 4, p0, dx, dy);
      rtcSetUserData(scene,geom0,(void*)123);
      rtcSetOcclusionFilterFunction(scene,geom0,intersectionFilter1);
#if defined(RTCORE_RAY_PACKETS)
      rtcSetOcclusionFilterFunction4(scene,geom0,intersectionFilter4);
      rtcSetOcclusionFilterFunction8(scene,geom0,intersectionFilter8);
      rtcSetOcclusionFilterFunction16(scene,geom0,intersectionFilter16);
#endif
      rtcCommit (scene);
      
      for (size_t iy=0; iy<4; iy++) 
      {
        for (size_t ix=0; ix<4; ix++) 
        {
          int primID = iy*4+ix;
          if (!subdiv) primID *= 2;
          
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            rtcOccluded(scene,ray0);
            bool ok0 = (primID & 2) ? (ray0.geomID == -1) : (ray0.geomID == 0);
            if (!ok0) passed = false;
          }
          
          if (subdiv) continue; // FIXME: subdiv filter callbacks only working for single ray queries
          
#if HAS_INTERSECT4
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
            setRay(ray4,0,ray0);
            __aligned(16) int valid4[4] = { -1,0,0,0 };
            rtcOccluded4(valid4,scene,ray4);
            bool ok0 = (primID & 2) ? (ray4.geomID[0] == -1) : (ray4.geomID[0] == 0);
            if (!ok0) passed = false;
          }
          
#if HAS_INTERSECT8
          if (hasISA(AVX))
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
            setRay(ray8,0,ray0);
            __aligned(32) int valid8[8] = { -1,0,0,0,0,0,0,0 };
            rtcOccluded8(valid8,scene,ray8);
            bool ok0 = (primID & 2) ? (ray8.geomID[0] == -1) : (ray8.geomID[0] == 0);
            if (!ok0) passed = false;
          }
#endif
          
#endif
          
#if HAS_INTERSECT16
          if (hasISA(AVX512KNL) || hasISA(KNC))
          {
            RTCRay ray0 = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
            
            RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
            setRay(ray16,0,ray0);
            __aligned(64) int valid16[16] = { -1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
            rtcOccluded16(valid16,scene,ray16);
            bool ok0 = (primID & 2) ? (ray16.geomID[0] == -1) : (ray16.geomID[0] == 0);
            if (!ok0) passed = false;
          }
#endif
        }
      }
      scene = nullptr;
      return passed;
    }
    
    bool run(VerifyApplication* state)
    {
      bool passed = true;
      for (int i=0; i<numSceneFlags; i++) 
      {
        RTCSceneFlags flag = getSceneFlag(i);
        bool ok0 = rtcore_filter_intersect(state,flag,RTC_GEOMETRY_STATIC, subdiv);
        if (ok0) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok0;
        bool ok1 = rtcore_filter_occluded(state,flag,RTC_GEOMETRY_STATIC, subdiv);
        if (ok1) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok1;
        fflush(stdout);
      }
      return passed;
    }
  };

  struct PacketWriteTest : public VerifyApplication::Test
  {
    PacketWriteTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
   
    bool rtcore_packet_write_test(VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags, int type)
    {
      bool passed = true;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      
      switch (type) {
      case 0: addSphere(state->device,scene,gflags,Vec3fa(-1,0,-1),1.0f,50,-1,0.0f); break;
      case 1: addSphere(state->device,scene,gflags,Vec3fa(-1,0,-1),1.0f,50,-1,0.1f); break;
      case 2: addHair  (state->device,scene,gflags,Vec3fa(-1,0,-1),1.0f,1.0f,1,0.0f); break;
      case 3: addHair  (state->device,scene,gflags,Vec3fa(-1,0,-1),1.0f,1.0f,1,0.1f); break; 
      }
      rtcCommit (scene);
      
      for (size_t i=0; i<4; i++) 
      {
        RTCRay ray = makeRay(Vec3fa(-1,10,-1),Vec3fa(0,-1,0));
        
#if HAS_INTERSECT4
        RTCRay4 ray4; memset(&ray4,-1,sizeof(RTCRay4));
        setRay(ray4,i,ray);
        __aligned(16) int valid4[4] = { 0,0,0,0 };
        valid4[i] = -1;
        rtcOccluded4(valid4,scene,ray4);
        rtcIntersect4(valid4,scene,ray4);
        
        for (int j=0; j<sizeof(RTCRay4)/4; j++) {
          if ((j%4) == i) continue;
          passed &= ((int*)&ray4)[j] == -1;
        }
#endif
        
#if HAS_INTERSECT8
        if (hasISA(AVX)) {
          RTCRay8 ray8; memset(&ray8,-1,sizeof(RTCRay8));
          setRay(ray8,i,ray);
          __aligned(32) int valid8[8] = { 0,0,0,0,0,0,0,0 };
          valid8[i] = -1;
          rtcOccluded8(valid8,scene,ray8);
          rtcIntersect8(valid8,scene,ray8);
          
          for (int j=0; j<sizeof(RTCRay8)/4; j++) {
            if ((j%8) == i) continue;
            passed &= ((int*)&ray8)[j] == -1;
          }
        }
#endif
        
#if HAS_INTERSECT16
        if (hasISA(AVX512KNL) || hasISA(KNC))
        {
          __aligned(64) RTCRay16 ray16; memset(&ray16,-1,sizeof(RTCRay16));
          setRay(ray16,i,ray);
          __aligned(64) int valid16[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
          valid16[i] = -1;
          rtcOccluded16(valid16,scene,ray16);
          rtcIntersect16(valid16,scene,ray16);
          
          for (int j=0; j<sizeof(RTCRay16)/4; j++) {
            if ((j%16) == i) continue;
            passed &= ((int*)&ray16)[j] == -1;
          }
        }
#endif
      }
      return passed;
    }
    
    bool run(VerifyApplication* state)
    {
      bool passed = true;
      for (int i=0; i<numSceneFlags; i++) 
      {
        RTCSceneFlags flag = getSceneFlag(i);
        bool ok = true;
        ok &= rtcore_packet_write_test(state,flag,RTC_GEOMETRY_STATIC,0);
        ok &= rtcore_packet_write_test(state,flag,RTC_GEOMETRY_STATIC,1);
        ok &= rtcore_packet_write_test(state,flag,RTC_GEOMETRY_STATIC,2);
        ok &= rtcore_packet_write_test(state,flag,RTC_GEOMETRY_STATIC,3);
        if (ok) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok;
        fflush(stdout);
      }
      return passed;
    }
  };

  struct Watertight1Test : public VerifyApplication::Test
  {
    ALIGNED_STRUCT;
    std::string type;
    Vec3fa pos;
    static const size_t testN = 100000;
    
    Watertight1Test (std::string name, std::string type, const Vec3fa& pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), type(type), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      if      (type == "sphere") addSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,500);
      else if (type == "cube"  ) addCube  (state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f);
      rtcCommit (scene);
      
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(pos+org,dir); 
        rtcIntersect(scene,ray);
        numFailures += ray.primID == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
  
#if defined(RTCORE_RAY_PACKETS)
    struct Watertight4Test : public VerifyApplication::Test
    {
      ALIGNED_STRUCT;
      std::string type;
      Vec3fa pos;
      static const size_t testN = 100000;
      
      Watertight4Test (std::string name, std::string type, const Vec3fa& pos)
        : VerifyApplication::Test(name,VerifyApplication::PASS), type(type), pos(pos) {}
      
      bool run(VerifyApplication* state)
      {
        ClearBuffers clear_before_return;
        RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
        if      (type == "sphere") addSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,500);
        else if (type == "cube"  ) addCube  (state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f);
        rtcCommit (scene);
        
        size_t numFailures = 0;
        for (size_t i=0; i<testN; i+=4) {
          RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
          for (size_t j=0; j<4; j++) {
            Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
            Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
            RTCRay ray = makeRay(pos+org,dir); 
            setRay(ray4,j,ray);
          }
          __aligned(16) int valid[4] = { -1,-1,-1,-1 };
          rtcIntersect4(valid,scene,ray4);
          for (size_t j=0; j<4; j++)
            numFailures += ray4.primID[j] == -1;
        }
        scene = nullptr;
        double failRate = double(numFailures) / double(testN);
        bool failed = failRate > 0.00002;
        
        printf(" (%f%%)", 100.0f*failRate);
        fflush(stdout);
        return !failed;
      }
    };
#endif

#if defined(RTCORE_RAY_PACKETS)
  struct Watertight8Test : public VerifyApplication::Test
  {
    ALIGNED_STRUCT;
    std::string type;
    Vec3fa pos;
    static const size_t testN = 100000;
    
    Watertight8Test (std::string name, std::string type, const Vec3fa& pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), type(type), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      if      (type == "sphere") addSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,500);
      else if (type == "cube"  ) addCube  (state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f);
      rtcCommit (scene);
      
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i+=8) {
        RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
        for (size_t j=0; j<8; j++) {
          Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          RTCRay ray = makeRay(pos+org,dir); 
          setRay(ray8,j,ray);
        }
        __aligned(32) int valid[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
        rtcIntersect8(valid,scene,ray8);
        for (size_t j=0; j<8; j++)
          numFailures += ray8.primID[j] == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
#endif

#if defined(RTCORE_RAY_PACKETS)
  struct Watertight16Test : public VerifyApplication::Test
  {
    ALIGNED_STRUCT;
    std::string type;
    Vec3fa pos;
    static const size_t testN = 100000;
    
    Watertight16Test (std::string name, std::string type, const Vec3fa& pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), type(type), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      if      (type == "sphere") addSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,500);
      else if (type == "cube"  ) addCube  (state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f);
      rtcCommit (scene);
      
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i+=16) {
        RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
        for (size_t j=0; j<16; j++) {
          Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          RTCRay ray = makeRay(pos+org,dir); 
          setRay(ray16,j,ray);
        }
        __aligned(64) int valid[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
        rtcIntersect16(valid,scene,ray16);
        for (size_t j=0; j<16; j++)
          numFailures += ray16.primID[j] == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)\n", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
#endif

  struct WatertightPlane1Test : public VerifyApplication::Test
  {
    float pos;
    static const size_t testN = 100000;
    
    WatertightPlane1Test (std::string name, const float pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      unsigned geom = addPlane(state->device,scene,RTC_GEOMETRY_STATIC,500,Vec3fa(pos,-6.0f,-6.0f),Vec3fa(0.0f,12.0f,0.0f),Vec3fa(0.0f,0.0f,12.0f));
      rtcCommit (scene);
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i++) {
        Vec3fa org(drand48()-0.5f,drand48()-0.5f,drand48()-0.5f);
        Vec3fa dir(1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(Vec3fa(pos-3.0f,0.0f,0.0f),dir); 
        rtcIntersect(scene,ray);
        numFailures += ray.primID == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
  
#if defined(RTCORE_RAY_PACKETS)
  struct WatertightPlane4Test : public VerifyApplication::Test
  {
    float pos;
    static const size_t testN = 100000;
    
    WatertightPlane4Test (std::string name, const float pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      unsigned geom = addPlane(state->device,scene,RTC_GEOMETRY_STATIC,500,Vec3fa(pos,-6.0f,-6.0f),Vec3fa(0.0f,12.0f,0.0f),Vec3fa(0.0f,0.0f,12.0f));
      rtcCommit (scene);
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i+=4) {
        RTCRay4 ray4; memset(&ray4,0,sizeof(ray4));
        for (size_t j=0; j<4; j++) {
          Vec3fa org(drand48()-0.5f,drand48()-0.5f,drand48()-0.5f);
          Vec3fa dir(1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          RTCRay ray = makeRay(Vec3fa(pos-3.0f,0.0f,0.0f),dir); 
          setRay(ray4,j,ray);
        }
        __aligned(16) int valid[4] = { -1,-1,-1,-1 };
        rtcIntersect4(valid,scene,ray4);
        for (size_t j=0; j<4; j++)
          numFailures += ray4.primID[j] == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
#endif
    
#if defined(RTCORE_RAY_PACKETS)
  struct WatertightPlane8Test : public VerifyApplication::Test
  {
    float pos;
    static const size_t testN = 100000;
    
    WatertightPlane8Test (std::string name, const float pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      unsigned geom = addPlane(state->device,scene,RTC_GEOMETRY_STATIC,500,Vec3fa(pos,-6.0f,-6.0f),Vec3fa(0.0f,12.0f,0.0f),Vec3fa(0.0f,0.0f,12.0f));
      rtcCommit (scene);
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i+=8) {
        RTCRay8 ray8; memset(&ray8,0,sizeof(ray8));
        for (size_t j=0; j<8; j++) {
          Vec3fa org(drand48()-0.5f,drand48()-0.5f,drand48()-0.5f);
          Vec3fa dir(1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          RTCRay ray = makeRay(Vec3fa(pos-3.0f,0.0f,0.0f),dir); 
          setRay(ray8,j,ray);
        }
        __aligned(32) int valid[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
        rtcIntersect8(valid,scene,ray8);
        for (size_t j=0; j<8; j++)
          numFailures += ray8.primID[j] == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
#endif
    
#if defined(RTCORE_RAY_PACKETS)
  struct WatertightPlane16Test : public VerifyApplication::Test
  {
    float pos;
    static const size_t testN = 100000;
    
    WatertightPlane16Test (std::string name, const float pos)
      : VerifyApplication::Test(name,VerifyApplication::PASS), pos(pos) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC | RTC_SCENE_ROBUST,aflags);
      unsigned geom = addPlane(state->device,scene,RTC_GEOMETRY_STATIC,500,Vec3fa(pos,-6.0f,-6.0f),Vec3fa(0.0f,12.0f,0.0f),Vec3fa(0.0f,0.0f,12.0f));
      rtcCommit (scene);
      size_t numFailures = 0;
      for (size_t i=0; i<testN; i+=16) {
        RTCRay16 ray16; memset(&ray16,0,sizeof(ray16));
        for (size_t j=0; j<16; j++) {
          Vec3fa org(drand48()-0.5f,drand48()-0.5f,drand48()-0.5f);
          Vec3fa dir(1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
          RTCRay ray = makeRay(Vec3fa(pos-3.0f,0.0f,0.0f),dir); 
          setRay(ray16,j,ray);
        }
        __aligned(64) int valid[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
        rtcIntersect16(valid,scene,ray16);
        for (size_t j=0; j<16; j++)
          numFailures += ray16.primID[j] == -1;
      }
      scene = nullptr;
      double failRate = double(numFailures) / double(testN);
      bool failed = failRate > 0.00002;
      
      printf(" (%f%%)", 100.0f*failRate);
      fflush(stdout);
      return !failed;
    }
  };
#endif
    
  struct NaNTest : public VerifyApplication::Test
  {
    RTCSceneFlags sflags;
    RTCGeometryFlags gflags;
    int N;
    
    NaNTest (std::string name, RTCSceneFlags sflags, RTCGeometryFlags gflags, int N)
      : VerifyApplication::Test(name,VerifyApplication::PASS), sflags(sflags), gflags(gflags), N(N) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      size_t count = 1000/N;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      addSphere(state->device,scene,gflags,zero,2.0f,100);
      addHair  (state->device,scene,gflags,zero,1.0f,1.0f,100);
      rtcCommit (scene);
      size_t numFailures = 0;
      double c0 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org,dir); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c1 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org+Vec3fa(nan),dir); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c2 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org+Vec3fa(nan),dir+Vec3fa(nan)); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c3 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org,dir,nan,nan); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c4 = getSeconds();
      
      double d1 = c1-c0;
      double d2 = c2-c1;
      double d3 = c3-c2;
      double d4 = c4-c3;
      scene = nullptr;
      
      bool ok = (d2 < 2.5*d1) && (d3 < 2.5*d1) && (d4 < 2.5*d1);
      float f = max(d2/d1,d3/d1,d4/d1);
      printf(" (%3.2fx)",f);
      fflush(stdout);
      return ok;
    }
  };
    
  struct InfTest : public VerifyApplication::Test
  {
    RTCSceneFlags sflags;
    RTCGeometryFlags gflags;
    int N;
    
    InfTest (std::string name, RTCSceneFlags sflags, RTCGeometryFlags gflags, int N)
      : VerifyApplication::Test(name,VerifyApplication::PASS), sflags(sflags), gflags(gflags), N(N) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      size_t count = 1000/N;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      addSphere(state->device,scene,gflags,zero,2.0f,100);
      addHair  (state->device,scene,gflags,zero,1.0f,1.0f,100);
      rtcCommit (scene);
      size_t numFailures = 0;
      double c0 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org,dir); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c1 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org+Vec3fa(inf),dir); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c2 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org,dir+Vec3fa(inf)); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c3 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org+Vec3fa(inf),dir+Vec3fa(inf)); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c4 = getSeconds();
      for (size_t i=0; i<count; i++) {
        Vec3fa org(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        Vec3fa dir(2.0f*drand48()-1.0f,2.0f*drand48()-1.0f,2.0f*drand48()-1.0f);
        RTCRay ray = makeRay(org,dir,-0.0f,inf); 
        rtcOccludedN(scene,ray,N);
        rtcIntersectN(scene,ray,N);
      }
      double c5 = getSeconds();
      
      double d1 = c1-c0;
      double d2 = c2-c1;
      double d3 = c3-c2;
      double d4 = c4-c3;
      double d5 = c5-c4;
      
      scene = nullptr;
      
      bool ok = (d2 < 2.5*d1) && (d3 < 2.5*d1) && (d4 < 2.5*d1) && (d5 < 2.5*d1);
      float f = max(d2/d1,d3/d1,d4/d1,d5/d1);
      printf(" (%3.2fx)",f);
      fflush(stdout);
      return ok;
    }
  };

  struct OverlappingTrianglesTest : public VerifyApplication::Test
  {
    int N;
    
    OverlappingTrianglesTest (std::string name, int N)
      : VerifyApplication::Test(name,VerifyApplication::PASS), N(N) {}
    
    bool run(VerifyApplication* state)
    {
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,aflags);
      AssertNoError(state->device);
      rtcNewTriangleMesh (scene, RTC_GEOMETRY_STATIC, N, 3);
      AssertNoError(state->device);
      
      Vertex3fa* vertices = (Vertex3fa*) rtcMapBuffer(scene,0,RTC_VERTEX_BUFFER);
      vertices[0].x = 0.0f; vertices[0].y = 0.0f; vertices[0].z = 0.0f;
      vertices[1].x = 1.0f; vertices[1].y = 0.0f; vertices[1].z = 0.0f;
      vertices[2].x = 0.0f; vertices[2].y = 1.0f; vertices[2].z = 0.0f;
      rtcUnmapBuffer(scene,0,RTC_VERTEX_BUFFER);
      AssertNoError(state->device);
      
      Triangle* triangles = (Triangle*) rtcMapBuffer(scene,0,RTC_INDEX_BUFFER);
      for (size_t i=0; i<N; i++) {
        triangles[i].v0 = 0;
        triangles[i].v1 = 1;
        triangles[i].v2 = 2;
      }
      rtcUnmapBuffer(scene,0,RTC_INDEX_BUFFER);
      AssertNoError(state->device);
      
      rtcCommit (scene);
      AssertNoError(state->device);
      
      return true;
    }
  };
    
  struct OverlappingHairTest : public VerifyApplication::Test
  {
    int N;
    
    OverlappingHairTest (std::string name, int N)
      : VerifyApplication::Test(name,VerifyApplication::PASS), N(N) {}
    
    bool run(VerifyApplication* state)
    {
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_STATIC,aflags);
      AssertNoError(state->device);
      rtcNewHairGeometry (scene, RTC_GEOMETRY_STATIC, N, 4);
      AssertNoError(state->device);
      
      Vec3fa* vertices = (Vec3fa*) rtcMapBuffer(scene,0,RTC_VERTEX_BUFFER);
      vertices[0].x = 0.0f; vertices[0].y = 0.0f; vertices[0].z = 0.0f; vertices[0].w = 0.1f;
      vertices[1].x = 0.0f; vertices[1].y = 0.0f; vertices[1].z = 1.0f; vertices[1].w = 0.1f;
      vertices[2].x = 0.0f; vertices[2].y = 1.0f; vertices[2].z = 1.0f; vertices[2].w = 0.1f;
      vertices[3].x = 0.0f; vertices[3].y = 1.0f; vertices[3].z = 0.0f; vertices[3].w = 0.1f;
      rtcUnmapBuffer(scene,0,RTC_VERTEX_BUFFER);
      AssertNoError(state->device);
      
      int* indices = (int*) rtcMapBuffer(scene,0,RTC_INDEX_BUFFER);
      for (size_t i=0; i<N; i++) {
        indices[i] = 0;
      }
      rtcUnmapBuffer(scene,0,RTC_INDEX_BUFFER);
      AssertNoError(state->device);
      
      rtcCommit (scene);
      AssertNoError(state->device);
      
      return true;
    }
  };

  struct BackfaceCullingTest : public VerifyApplication::Test
  {
    BackfaceCullingTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool rtcore_backface_culling (VerifyApplication* state, RTCSceneFlags sflags, RTCGeometryFlags gflags)
    {
      /* create triangle that is front facing for a right handed 
         coordinate system if looking along the z direction */
      RTCSceneRef scene = rtcDeviceNewScene(state->device,sflags,aflags);
      unsigned mesh = rtcNewTriangleMesh (scene, gflags, 1, 3);
      Vertex3fa*   vertices  = (Vertex3fa*  ) rtcMapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
      Triangle* triangles = (Triangle*) rtcMapBuffer(scene,mesh,RTC_INDEX_BUFFER);
      vertices[0].x = 0; vertices[0].y = 0; vertices[0].z = 0;
      vertices[1].x = 0; vertices[1].y = 1; vertices[1].z = 0;
      vertices[2].x = 1; vertices[2].y = 0; vertices[2].z = 0;
      triangles[0].v0 = 0; triangles[0].v1 = 1; triangles[0].v2 = 2;
      rtcUnmapBuffer(scene,mesh,RTC_VERTEX_BUFFER); 
      rtcUnmapBuffer(scene,mesh,RTC_INDEX_BUFFER);
      rtcCommit (scene);
      
      bool passed = true;
      RTCRay ray;
      RTCRay backfacing = makeRay(Vec3fa(0.25f,0.25f,1),Vec3fa(0,0,-1)); 
      RTCRay frontfacing = makeRay(Vec3fa(0.25f,0.25f,-1),Vec3fa(0,0,1)); 
      
      ray = frontfacing; rtcOccludedN(scene,ray,1);  if (ray.geomID != 0) passed = false;
      ray = frontfacing; rtcIntersectN(scene,ray,1); if (ray.geomID != 0) passed = false;
      ray = backfacing;  rtcOccludedN(scene,ray,1);  if (ray.geomID != -1) passed = false;
      ray = backfacing;  rtcIntersectN(scene,ray,1); if (ray.geomID != -1) passed = false;
#if HAS_INTERSECT4
      ray = frontfacing; rtcOccludedN(scene,ray,4);  if (ray.geomID != 0) passed = false;
      ray = frontfacing; rtcIntersectN(scene,ray,4); if (ray.geomID != 0) passed = false;
      ray = backfacing;  rtcOccludedN(scene,ray,4);  if (ray.geomID != -1) passed = false;
      ray = backfacing;  rtcIntersectN(scene,ray,4); if (ray.geomID != -1) passed = false;
#endif
#if HAS_INTERSECT8
      if (hasISA(AVX)) {
        ray = frontfacing; rtcOccludedN(scene,ray,8);  if (ray.geomID != 0) passed = false;
        ray = frontfacing; rtcIntersectN(scene,ray,8); if (ray.geomID != 0) passed = false;
        ray = backfacing;  rtcOccludedN(scene,ray,8);  if (ray.geomID != -1) passed = false;
        ray = backfacing;  rtcIntersectN(scene,ray,8); if (ray.geomID != -1) passed = false;
      }
#endif
#if HAS_INTERSECT16
      if (hasISA(AVX512KNL) || hasISA(KNC))
      {
        ray = frontfacing; rtcOccludedN(scene,ray,16); if (ray.geomID != 0) passed = false;
        ray = frontfacing; rtcIntersectN(scene,ray,16);if (ray.geomID != 0) passed = false;
        ray = backfacing;  rtcOccludedN(scene,ray,16); if (ray.geomID != -1) passed = false;
        ray = backfacing;  rtcIntersectN(scene,ray,16);if (ray.geomID != -1) passed = false;
      }
#endif
      return passed;
    }
    
    bool run(VerifyApplication* state)
    {
      bool passed = true;
      for (int i=0; i<numSceneFlags; i++) 
      {
        RTCSceneFlags flag = getSceneFlag(i);
        bool ok0 = rtcore_backface_culling(state,flag,RTC_GEOMETRY_STATIC);
        if (ok0) printf(GREEN("+")); else printf(RED("-"));
        passed &= ok0;
        fflush(stdout);
      }
      return passed;
    }
  };

  struct NewDeleteGeometryTest : public VerifyApplication::Test
  {
    NewDeleteGeometryTest (std::string name)
      : VerifyApplication::Test(name,VerifyApplication::PASS) {}
    
    bool run(VerifyApplication* state)
    {
      ClearBuffers clear_before_return;
      RTCSceneRef scene = rtcDeviceNewScene(state->device,RTC_SCENE_DYNAMIC,aflags);
      AssertNoError(state->device);
      int geom[128];
      for (size_t i=0; i<128; i++) geom[i] = -1;
      Sphere spheres[128];
      memset(spheres,0,sizeof(spheres));
      
      for (size_t i=0; i<50; i++) {
        for (size_t j=0; j<10; j++) {
          int index = random<int>()%128;
          Vec3fa pos = 100.0f*Vec3fa(drand48(),drand48(),drand48());
          if (geom[index] == -1) {
            switch (random<int>()%4) {
            case 0: geom[index] = addSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,10); break;
            case 1: geom[index] = addHair  (state->device,scene,RTC_GEOMETRY_STATIC,pos,1.0f,2.0f,10); break;
            case 2: geom[index] = addSubdivSphere(state->device,scene,RTC_GEOMETRY_STATIC,pos,2.0f,4,4); break;
            case 3: 
              spheres[index] = Sphere(pos,2.0f);
              geom[index] = addUserGeometryEmpty(state->device,scene,&spheres[index]); break;
            }
            AssertNoError(state->device);
          }
          else { 
            rtcDeleteGeometry(scene,geom[index]);     
            AssertNoError(state->device);
            geom[index] = -1; 
          }
        }
        rtcCommit(scene);
        AssertNoError(state->device);
        rtcCommit(scene);
        AssertNoError(state->device);
        if (i%2 == 0) std::cout << "." << std::flush;
      }
      rtcCommit (scene);
      AssertNoError(state->device);
      scene = nullptr;
      return true;
    }
  };

  VerifyApplication::VerifyApplication ()
    : device(nullptr), rtcore(""), regressionN(200), numFailedTests(0)
  {
    /* add all tests */
    addTest(new MultipleDevicesTest("multiple_devices"));
    addTest(new EmptySceneTest("empty_static",RTC_SCENE_STATIC));
    addTest(new EmptySceneTest("empty_dynamic",RTC_SCENE_DYNAMIC));
    // FIXME: add test with empty meshes
    addTest(new BaryDistanceTest("bary_distance_robust"));

    addTest(new FlagsTest("flags_static_static"     ,VerifyApplication::PASS, RTC_SCENE_STATIC, RTC_GEOMETRY_STATIC));
    addTest(new FlagsTest("flags_static_deformable" ,VerifyApplication::FAIL, RTC_SCENE_STATIC, RTC_GEOMETRY_DEFORMABLE));
    addTest(new FlagsTest("flags_static_dynamic"    ,VerifyApplication::FAIL, RTC_SCENE_STATIC, RTC_GEOMETRY_DYNAMIC));
    addTest(new FlagsTest("flags_dynamic_static"    ,VerifyApplication::PASS, RTC_SCENE_DYNAMIC,RTC_GEOMETRY_STATIC));
    addTest(new FlagsTest("flags_dynamic_deformable",VerifyApplication::PASS, RTC_SCENE_DYNAMIC,RTC_GEOMETRY_DEFORMABLE));
    addTest(new FlagsTest("flags_dynamic_dynamic"   ,VerifyApplication::PASS, RTC_SCENE_DYNAMIC,RTC_GEOMETRY_DYNAMIC));
    
    addTest(new StaticSceneTest("static_scene"));
    addTest(new UnmappedBeforeCommitTest("unmapped_before_commit"));
    addTest(new GetBoundsTest("get_bounds"));

#if defined(RTCORE_BUFFER_STRIDE)
    addTest(new BufferStrideTest("buffer_stride"));
#endif

    addTest(new DynamicEnableDisableTest("dynamic_enable_disable"));
    addTest(new GetUserDataTest("get_user_data"));

    addTest(new UpdateTest("update_deformable",RTC_GEOMETRY_DEFORMABLE));
    addTest(new UpdateTest("update_dynamic",RTC_GEOMETRY_DYNAMIC));
    addTest(new OverlappingTrianglesTest("overlapping_triangles",100000));
    addTest(new OverlappingHairTest("overlapping_hair",100000));
    addTest(new NewDeleteGeometryTest("new_delete_geometry"));

    //POSITIVE("interpolate_subdiv4",                rtcore_interpolate_subdiv(4));
    //POSITIVE("interpolate_subdiv5",                rtcore_interpolate_subdiv(5));
    //POSITIVE("interpolate_subdiv8",                rtcore_interpolate_subdiv(8));
    //POSITIVE("interpolate_subdiv11",               rtcore_interpolate_subdiv(11));
    //POSITIVE("interpolate_subdiv12",               rtcore_interpolate_subdiv(12));
    //POSITIVE("interpolate_subdiv15",               rtcore_interpolate_subdiv(15));

    //POSITIVE("interpolate_triangles4",                rtcore_interpolate_triangles(4));
    //POSITIVE("interpolate_triangles5",                rtcore_interpolate_triangles(5));
    //POSITIVE("interpolate_triangles8",                rtcore_interpolate_triangles(8));
    //POSITIVE("interpolate_triangles11",               rtcore_interpolate_triangles(11));
    //POSITIVE("interpolate_triangles12",               rtcore_interpolate_triangles(12));
    //POSITIVE("interpolate_triangles15",               rtcore_interpolate_triangles(15));

    //POSITIVE("interpolate_hair4",                rtcore_interpolate_hair(4));
    //POSITIVE("interpolate_hair5",                rtcore_interpolate_hair(5));
    //POSITIVE("interpolate_hair8",                rtcore_interpolate_hair(8));
    //POSITIVE("interpolate_hair11",               rtcore_interpolate_hair(11));
    //POSITIVE("interpolate_hair12",               rtcore_interpolate_hair(12));
    //POSITIVE("interpolate_hair15",               rtcore_interpolate_hair(15));

    addTest(new BuildTest("build"));

#if defined(RTCORE_RAY_MASK)
    addTest(new RayMasksTest("ray_masks"));
#endif

#if defined(RTCORE_INTERSECTION_FILTER)
    addTest(new IntersectionFilterTest("intersection_filter_tris",false));
#endif

#if defined(RTCORE_INTERSECTION_FILTER)
    addTest(new IntersectionFilterTest("intersection_filter_subdiv",true));
#endif

#if defined(RTCORE_BACKFACE_CULLING)
    addTest(new BackfaceCullingTest("backface_culling"));
#endif

    addTest(new PacketWriteTest("packet_write_test"));

    const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
    addTest(new Watertight1Test("watertight_sphere_1","sphere",watertight_pos));
    addTest(new Watertight1Test("watertight_cube_1","cube",watertight_pos));
    addTest(new WatertightPlane1Test("watertight_plane_1",100000));
#if HAS_INTERSECT4
    addTest(new Watertight4Test("watertight_sphere_4","sphere",watertight_pos));
    addTest(new Watertight4Test("watertight_cube_4","cube",watertight_pos));
    addTest(new WatertightPlane4Test("watertight_plane_4",100000));
#endif

#if HAS_INTERSECT8
    if (hasISA(AVX)) {
      addTest(new Watertight8Test("watertight_sphere_8","sphere",watertight_pos));
      addTest(new Watertight8Test("watertight_cube_8","cube",watertight_pos));
      addTest(new WatertightPlane8Test("watertight_plane_8",100000));
    }
#endif

#if HAS_INTERSECT16
    if (hasISA(AVX512KNL) || hasISA(KNC))
    {
      addTest(new Watertight16Test("watertight_sphere_16","sphere",watertight_pos));
      addTest(new Watertight16Test("watertight_cube_16","cube",watertight_pos));
      addTest(new WatertightPlane16Test("watertight_plane_16",100000));
    }

#endif

#if defined(RTCORE_IGNORE_INVALID_RAYS)
    addTest(new NaNTest("nan_test_1",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,1));
    addTest(new InfTest("inf_test_1",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,1));

#if HAS_INTERSECT4
    addTest(new NaNTest("nan_test_4",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,4));
    addTest(new InfTest("inf_test_4",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,4));
#endif

#if HAS_INTERSECT8
    if (hasISA(AVX)) {
      addTest(new NaNTest("nan_test_8",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,8));
      addTest(new InfTest("inf_test_8",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,8));
    }
#endif

#if HAS_INTERSECT16
    if (hasISA(AVX512KNL) || hasISA(KNC))
    {
      addTest(new NaNTest("nan_test_16",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,16));
      addTest(new InfTest("inf_test_16",RTC_SCENE_STATIC,RTC_GEOMETRY_STATIC,16));
    }
#endif
#endif

    //POSITIVE("regression_static",         rtcore_regression(rtcore_regression_static_thread,0));
    //POSITIVE("regression_dynamic",        rtcore_regression(rtcore_regression_dynamic_thread,0));


#if defined(TASKING_TBB) || defined(TASKING_INTERNAL)
    //POSITIVE("regression_static_user_threads", rtcore_regression(rtcore_regression_static_thread,1));
    //POSITIVE("regression_dynamic_user_threads", rtcore_regression(rtcore_regression_dynamic_thread,1));
#endif

#if defined(TASKING_TBB) || defined(TASKING_INTERNAL)
    //POSITIVE("regression_static_build_join", rtcore_regression(rtcore_regression_static_thread,2));
    //POSITIVE("regression_dynamic_build_join", rtcore_regression(rtcore_regression_dynamic_thread,2));
#endif
      
#if defined(TASKING_TBB) || defined(TASKING_INTERNAL)
    //POSITIVE("regression_static_memory_monitor",  rtcore_regression_memory_monitor(rtcore_regression_static_thread));
    //POSITIVE("regression_dynamic_memory_monitor", rtcore_regression_memory_monitor(rtcore_regression_dynamic_thread));
#endif

#if !defined(__MIC__)
    //POSITIVE("regression_garbage_geom",   rtcore_regression_garbage());
#endif

    /* register all command line options*/
    registerOption("rtcore", [this] (Ref<ParseStream> cin, const FileName& path) {
        rtcore += "," + cin->getString();
      }, "--rtcore <string>: uses <string> to configure Embree device");
    
    registerOption("threads", [this] (Ref<ParseStream> cin, const FileName& path) {
        rtcore += ",threads=" + toString(cin->getInt());
      }, "--threads <int>: number of threads to use");
    
    registerOption("affinity", [this] (Ref<ParseStream> cin, const FileName& path) {
        rtcore += ",set_affinity=1";
      }, "--affinity: affinitize threads");
    
    registerOption("verbose", [this] (Ref<ParseStream> cin, const FileName& path) {
        rtcore += ",verbose=" + toString(cin->getInt());
      }, "--verbose <int>: sets verbosity level");
    
    std::string run_docu = "--run testname: runs specified test, supported tests are:";
    for (auto test : tests) run_docu += "\n  " + test->name;
    registerOption("run", [this] (Ref<ParseStream> cin, const FileName& path) {
        std::string name = cin->getString();
        if (name2test.find(name) == name2test.end()) throw std::runtime_error("Unknown test: "+name);
        tests_to_run.push_back(name2test[name]);
      }, run_docu);
    
    registerOption("regressions", [this] (Ref<ParseStream> cin, const FileName& path) {
        regressionN = cin->getInt();
      }, "--regressions <int>: number of regressions to perform");
  }

  int VerifyApplication::main(int argc, char** argv) try
  {
    /* for best performance set FTZ and DAZ flags in MXCSR control and status register */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    
    /* parse command line options */
    parseCommandLine(argc,argv);
    
    /* print Embree version */
    rtcInit("verbose=1");
    error_handler(rtcGetError());
    rtcExit();
    
    /* perform tests */
    device = rtcNewDevice(rtcore.c_str());
    error_handler(rtcDeviceGetError(device));

    /* execute specific user tests */
    if (tests_to_run.size()) {
      for (auto test : tests_to_run) runTest(test);
    } else {
      for (auto test : tests) runTest(test);
    }

    rtcDeleteDevice(device);
    return numFailedTests;
  }
  catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
    return 1;
  }
  catch (...) {
    std::cout << "Error: unknown exception caught." << std::endl;
    return 1;
  }

  void VerifyApplication::addTest(Ref<Test> test) 
  {
    tests.push_back(test);
    name2test[test->name] = test;
  }
  
  void VerifyApplication::runTest(Ref<Test> test)
  {
    bool ok = true;
    std::cout << std::setw(30) << test->name << " ..." << std::flush;
    try {
      ok &= test->run(this);
    } catch (...) {
      ok = false;
    }
    if ((test->ty == PASS) == ok) 
      std::cout << GREEN(" [PASSED]") << std::endl << std::flush;
    else {
      std::cout << RED(" [FAILED]") << std::endl << std::flush;
      numFailedTests++;
    }
  }
}

int main(int argc, char** argv)
{
  embree::VerifyApplication app;
  return app.main(argc,argv);
}
