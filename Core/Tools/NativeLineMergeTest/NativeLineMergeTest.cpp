#include "Utility/CppMacros.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"

// Minimal CPU fixture for the names consumed by the extracted production blocks.
struct Intersection {
    unsigned PointCount;
    Vector3 Point;
    float TexV;
    Vector4 RGBA;
    Vector3 Direction;
    bool Parallel;
};
struct Segment { Vector3 EdgePlane[1]; };
struct MergeResult {
    unsigned count;
    Vector3 point;
    float texV;
    Vector4 rgba;
    Vector3 direction;
    bool parallel;
};
#include "segline.inc"
#include "streak.inc"
using Producer=MergeResult(*)(const Intersection&,const Intersection&,const Segment&,const Segment&,const Segment&,const Vector3&);

static void Require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
static void Near(float actual,float expected,const char* message) {
    Require(std::isfinite(actual) && std::fabs(actual-expected)<0.00002f,message);
}
static void Near3(const Vector3& actual,const Vector3& expected,const char* message) {
    for(int i=0;i<3;++i) Near(actual[i],expected[i],message);
}
static Vector3 Poison() {
    const std::uint32_t bits=0xffffffffu;
    float component;
    std::memcpy(&component,&bits,sizeof(component));
    return Vector3(component,component,component);
}
static void CheckProducer(Producer merge,bool hasRGBA) {
    const Segment previous{{Vector3(0,0,1)}};
    const Segment skipped{{Vector3(1,0,0)}};
    const Segment following{{Vector3(0,0,1)}};
    Intersection a{1,Vector3(2,4,6),0.2f,Vector4(0.1f,0.2f,0.3f,0.4f),Vector3(3,4,2),true};
    Intersection b{3,Vector3(10,12,14),0.8f,Vector4(0.9f,0.6f,0.7f,0.8f),Vector3(1,0,0),true};
    for(unsigned nextCount : {3u,1u}) {
        b.PointCount=nextCount;
        const auto result=merge(a,b,previous,skipped,following,Poison());
        Require(result.count==1+nextCount,"merged count");
        const Vector3 expected=nextCount==3 ? Vector3(8,10,12) : Vector3(6,8,10);
        Near3(result.point,expected,"weighted point");
        Near(result.texV,nextCount==3 ? 0.65f : 0.5f,"weighted texture V");
        if(hasRGBA) {
            const Vector4 expectedRGBA=nextCount==3 ? Vector4(0.7f,0.5f,0.6f,0.7f) : Vector4(0.5f,0.4f,0.5f,0.6f);
            for(int i=0;i<4;++i) Near(result.rgba[i],expectedRGBA[i],"weighted RGBA");
        }
    }
    a.PointCount=3;
    b.PointCount=1;
    const auto reversed=merge(a,b,previous,skipped,following,Poison());
    Require(reversed.count==4,"reversed merged count");
    Near3(reversed.point,Vector3(4,6,8),"reversed weighted point");
    Near(reversed.texV,0.35f,"reversed weighted texture V");
    if(hasRGBA) {
        const Vector4 expectedRGBA(0.3f,0.3f,0.4f,0.5f);
        for(int i=0;i<4;++i) Near(reversed.rgba[i],expectedRGBA[i],"reversed weighted RGBA");
    }
    a.PointCount=1;
    // Aligned and opposed outer planes select the same stable average normal.
    // Both seed patterns must be overwritten before projection/normalization.
    for(float sign : {1.0f,-1.0f}) {
        const Segment outer{{Vector3(0,0,sign)}};
        for(const Vector3 seed : {Poison(),Vector3(-9,17,23)}) {
            const auto result=merge(a,b,previous,skipped,outer,seed);
            Require(result.parallel,"parallel branch");
            Near3(result.direction,Vector3(0.6f,0.8f,0),"projection direction");
            Near(Vector3::Dot_Product(previous.EdgePlane[0],result.direction),0,"tangent projection");
            Near(result.direction.Length2(),1,"normalized projection");
            a.Direction=Vector3(0,0,2);
            const auto zero=merge(a,b,previous,skipped,outer,seed);
            Near3(zero.direction,Vector3(0,0,0),"zero projection stays finite zero");
            a.Direction=Vector3(3,4,2);
        }
    }
    // Dot 0.95 falls within the production near-parallel branch (threshold 0.9).
    // A substituted 0.999 threshold would instead select the cross-product path.
    const Segment nearParallel{{Vector3(std::sqrt(1.0f-0.95f*0.95f),0,0.95f)}};
    const auto nearResult=merge(a,b,previous,skipped,nearParallel,Poison());
    Require(nearResult.parallel,"production near-parallel threshold");
    Near(nearResult.direction.Length2(),1,"near-parallel normalized projection");
    Near(Vector3::Dot_Product(previous.EdgePlane[0]+nearParallel.EdgePlane[0],nearResult.direction),0,"near-parallel tangent projection");
    // Retain the neighboring current-nonparallel and outer-nonparallel branches.
    a.Parallel=false;
    const auto crossed=merge(a,b,previous,skipped,following,Poison());
    Near3(crossed.direction,Vector3(0,-1,0),"skipped-plane cross product");
    const Segment perpendicular{{Vector3(1,0,0)}};
    const auto nonparallel=merge(a,b,previous,skipped,perpendicular,Poison());
    Require(!nonparallel.parallel,"outer nonparallel branch");
    Near3(nonparallel.direction,Vector3(0,1,0),"outer-plane cross product orientation");
}
int main() {
    try {
        CheckProducer(Merge_segline,true);
        CheckProducer(Merge_streak,false);
        std::cout << "Production SegLine/Streak weighted means and direction branches passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
