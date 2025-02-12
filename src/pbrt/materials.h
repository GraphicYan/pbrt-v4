// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#ifndef PBRT_MATERIALS_H
#define PBRT_MATERIALS_H

#include <pbrt/pbrt.h>

#include <pbrt/base/bssrdf.h>
#include <pbrt/base/material.h>
#include <pbrt/bsdf.h>
#include <pbrt/bssrdf.h>
#include <pbrt/interaction.h>
#include <pbrt/textures.h>
#include <pbrt/util/check.h>
#include <pbrt/util/spectrum.h>
#include <pbrt/util/taggedptr.h>
#include <pbrt/util/transform.h>

#include <memory>
#include <string>
#include <type_traits>

namespace pbrt {

// MaterialEvalContext Definition
struct MaterialEvalContext : public TextureEvalContext {
    // MaterialEvalContext Public Methods
    MaterialEvalContext() = default;
    PBRT_CPU_GPU
    MaterialEvalContext(const SurfaceInteraction &si)
        : TextureEvalContext(si),
          wo(si.wo),
          n(si.n),
          ns(si.shading.n),
          dpdus(si.shading.dpdu) {}
    std::string ToString() const;

    Vector3f wo;
    Normal3f n, ns;
    Vector3f dpdus;
};

// BumpEvalContext Definition
struct BumpEvalContext {
    // BumpEvalContext Public Methods
    BumpEvalContext() = default;
    PBRT_CPU_GPU
    BumpEvalContext(const SurfaceInteraction &si)
        : p(si.p()),
          uv(si.uv),
          dudx(si.dudx),
          dudy(si.dudy),
          dvdx(si.dvdx),
          dvdy(si.dvdy),
          dpdx(si.dpdx),
          dpdy(si.dpdy),
          faceIndex(si.faceIndex) {
        shading.n = si.shading.n;
        shading.dpdu = si.shading.dpdu;
        shading.dpdv = si.shading.dpdv;
        shading.dndu = si.shading.dndu;
        shading.dndv = si.shading.dndv;
    }
    std::string ToString() const;

    PBRT_CPU_GPU
    operator TextureEvalContext() const {
        return TextureEvalContext(p, dpdx, dpdy, uv, dudx, dudy, dvdx, dvdy, faceIndex);
    }

    // BumpEvalContext Public Members
    Point3f p;
    Point2f uv;
    struct {
        Normal3f n;
        Vector3f dpdu, dpdv;
        Normal3f dndu, dndv;
    } shading;
    Float dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
    Vector3f dpdx, dpdy;
    int faceIndex = 0;
};

// Bump-mapping Function Definitions
template <typename TextureEvaluator>
PBRT_CPU_GPU void Bump(TextureEvaluator texEval, FloatTexture displacement,
                       const Image *normalMap, const BumpEvalContext &ctx, Vector3f *dpdu,
                       Vector3f *dpdv) {
    DCHECK(displacement != nullptr || normalMap != nullptr);
    if (displacement) {
        if (displacement)
            DCHECK(texEval.CanEvaluate({displacement}, {}));
        // Compute offset positions and evaluate displacement texture
        TextureEvalContext shiftedCtx = ctx;
        // Shift _shiftedCtx_ _du_ in the $u$ direction
        Float du = .5f * (std::abs(ctx.dudx) + std::abs(ctx.dudy));
        if (du == 0)
            du = .0005f;
        shiftedCtx.p = ctx.p + du * ctx.shading.dpdu;
        shiftedCtx.uv = ctx.uv + Vector2f(du, 0.f);

        Float uDisplace = texEval(displacement, shiftedCtx);
        // Shift _shiftedCtx_ _dv_ in the $v$ direction
        Float dv = .5f * (std::abs(ctx.dvdx) + std::abs(ctx.dvdy));
        if (dv == 0)
            dv = .0005f;
        shiftedCtx.p = ctx.p + dv * ctx.shading.dpdv;
        shiftedCtx.uv = ctx.uv + Vector2f(0.f, dv);

        Float vDisplace = texEval(displacement, shiftedCtx);
        Float displace = texEval(displacement, ctx);

        // Compute bump-mapped differential geometry
        *dpdu = ctx.shading.dpdu + (uDisplace - displace) / du * Vector3f(ctx.shading.n) +
                displace * Vector3f(ctx.shading.dndu);
        *dpdv = ctx.shading.dpdv + (vDisplace - displace) / dv * Vector3f(ctx.shading.n) +
                displace * Vector3f(ctx.shading.dndv);

    } else {
        // Sample normal map to compute shading normal
        WrapMode2D wrap(WrapMode::Repeat);
        Point2f uv(ctx.uv[0], 1 - ctx.uv[1]);
        Vector3f ns(2 * normalMap->BilerpChannel(uv, 0, wrap) - 1,
                    2 * normalMap->BilerpChannel(uv, 1, wrap) - 1,
                    2 * normalMap->BilerpChannel(uv, 2, wrap) - 1);
        ns = Normalize(ns);
        Frame frame = Frame::FromZ(ctx.shading.n);
        ns = frame.FromLocal(ns);

        Float ulen = Length(ctx.shading.dpdu), vlen = Length(ctx.shading.dpdv);
        *dpdu = Normalize(GramSchmidt(ctx.shading.dpdu, ns)) * ulen;
        *dpdv = Normalize(Cross(ns, *dpdu)) * vlen;
    }
}

// DielectricMaterial Definition
class DielectricMaterial {
  public:
    // DielectricMaterial Type Definitions
    using BxDF = DielectricBxDF;
    using BSSRDF = void;

    // DielectricMaterial Public Methods
    DielectricMaterial(FloatTexture uRoughness, FloatTexture vRoughness, Spectrum eta,
                       FloatTexture displacement, Image *normalMap, bool remapRoughness)
        : displacement(displacement),
          normalMap(normalMap),
          uRoughness(uRoughness),
          vRoughness(vRoughness),
          eta(eta),
          remapRoughness(remapRoughness) {}

    static const char *Name() { return "DielectricMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({uRoughness, vRoughness}, {});
    }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static DielectricMaterial *Create(const TextureParameterDictionary &parameters,
                                      Image *normalMap, const FileLoc *loc,
                                      Allocator alloc);

    std::string ToString() const;

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, DielectricBxDF *bxdf) const {
        // Compute index of refraction for dielectric material
        Float sampledEta = eta(lambda[0]);
        if (!eta.template Is<ConstantSpectrum>())
            lambda.TerminateSecondary();
        if (sampledEta == 0)
            sampledEta = 1;

        // Create microfacet distribution for dielectric material
        Float urough = texEval(uRoughness, ctx), vrough = texEval(vRoughness, ctx);
        if (remapRoughness) {
            urough = TrowbridgeReitzDistribution::RoughnessToAlpha(urough);
            vrough = TrowbridgeReitzDistribution::RoughnessToAlpha(vrough);
        }
        TrowbridgeReitzDistribution distrib(urough, vrough);

        // Return BSDF for dielectric material
        *bxdf = DielectricBxDF(sampledEta, distrib);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

  private:
    // DielectricMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    FloatTexture uRoughness, vRoughness;
    Spectrum eta;
    bool remapRoughness;
};

// ThinDielectricMaterial Definition
class ThinDielectricMaterial {
  public:
    using BxDF = ThinDielectricBxDF;
    using BSSRDF = void;
    // ThinDielectricMaterial Public Methods
    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return true;
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda,
                              ThinDielectricBxDF *bxdf) const {
        // Compute index of refraction for dielectric material
        Float sampledEta = eta(lambda[0]);
        if (!eta.template Is<ConstantSpectrum>())
            lambda.TerminateSecondary();
        if (sampledEta == 0)
            sampledEta = 1;

        // Return BSDF for _ThinDielectricMaterial_
        *bxdf = ThinDielectricBxDF(sampledEta);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    ThinDielectricMaterial(Spectrum eta, FloatTexture displacement, Image *normalMap)
        : displacement(displacement), normalMap(normalMap), eta(eta) {}

    static const char *Name() { return "ThinDielectricMaterial"; }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static ThinDielectricMaterial *Create(const TextureParameterDictionary &parameters,
                                          Image *normalMap, const FileLoc *loc,
                                          Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // ThinDielectricMaterial Private Data
    FloatTexture displacement;
    Image *normalMap;
    Spectrum eta;
};

// MixMaterial Definition
class MixMaterial {
  public:
    // MixMaterial Type Definitions
    using BxDF = int;
    using BSSRDF = void;

    // MixMaterial Public Methods
    MixMaterial(Material m[2], FloatTexture amount) : amount(amount) {
        materials[0] = m[0];
        materials[1] = m[1];
    }

    PBRT_CPU_GPU
    Material GetMaterial(int i) const { return materials[i]; }

    static const char *Name() { return "MixMaterial"; }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const {
#ifndef PBRT_IS_GPU_CODE
        LOG_FATAL("Shouldn't be called");
#endif
        return nullptr;
    }

    PBRT_CPU_GPU
    const Image *GetNormalMap() const {
#ifndef PBRT_IS_GPU_CODE
        LOG_FATAL("Shouldn't be called");
#endif
        return nullptr;
    }

    static MixMaterial *Create(Material materials[2],
                               const TextureParameterDictionary &parameters,
                               const FileLoc *loc, Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {
#ifndef PBRT_IS_GPU_CODE
        LOG_FATAL("Shouldn't be called");
#endif
    }

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({amount}, {});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU Material ChooseMaterial(TextureEvaluator texEval,
                                         MaterialEvalContext ctx) const {
        Float amt = texEval(amount, ctx);
        if (amt <= 0)
            return materials[0];
        if (amt >= 1)
            return materials[1];

        Float u = HashFloat(ctx.p, ctx.wo, materials[0], materials[1]);
        return (amt < u) ? materials[0] : materials[1];
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, void *bxdf) const {
#ifndef PBRT_IS_GPU_CODE
        LOG_FATAL("MixMaterial::GetBSDF() shouldn't be called");
#endif
        return {};
    }

  private:
    // MixMaterial Private Members
    FloatTexture amount;
    Material materials[2];
};

// HairMaterial Definition
class HairMaterial {
  public:
    using BxDF = HairBxDF;
    using BSSRDF = void;

    // HairMaterial Public Methods
    HairMaterial(SpectrumTexture sigma_a, SpectrumTexture color, FloatTexture eumelanin,
                 FloatTexture pheomelanin, FloatTexture eta, FloatTexture beta_m,
                 FloatTexture beta_n, FloatTexture alpha)
        : sigma_a(sigma_a),
          color(color),
          eumelanin(eumelanin),
          pheomelanin(pheomelanin),
          eta(eta),
          beta_m(beta_m),
          beta_n(beta_n),
          alpha(alpha) {}

    static const char *Name() { return "HairMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({eumelanin, pheomelanin, eta, beta_m, beta_n, alpha},
                                   {sigma_a, color});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, HairBxDF *bxdf) const {
        Float bm = std::max<Float>(1e-2, texEval(beta_m, ctx));
        Float bn = std::max<Float>(1e-2, texEval(beta_n, ctx));
        Float a = texEval(alpha, ctx);
        Float e = texEval(eta, ctx);

        SampledSpectrum sig_a;
        if (sigma_a)
            sig_a = ClampZero(texEval(sigma_a, ctx, lambda));
        else if (color) {
            SampledSpectrum c = Clamp(texEval(color, ctx, lambda), 0, 1);
            sig_a = HairBxDF::SigmaAFromReflectance(c, bn, lambda);
        } else {
            CHECK(eumelanin || pheomelanin);
            sig_a = HairBxDF::SigmaAFromConcentration(
                        std::max(Float(0), eumelanin ? texEval(eumelanin, ctx) : 0),
                        std::max(Float(0), pheomelanin ? texEval(pheomelanin, ctx) : 0))
                        .Sample(lambda);
        }

        // Offset along width
        Float h = -1 + 2 * ctx.uv[1];
        *bxdf = HairBxDF(h, e, sig_a, bm, bn, a);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    static HairMaterial *Create(const TextureParameterDictionary &parameters,
                                const FileLoc *loc, Allocator alloc);

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return nullptr; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return nullptr; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // HairMaterial Private Data
    SpectrumTexture sigma_a, color;
    FloatTexture eumelanin, pheomelanin, eta;
    FloatTexture beta_m, beta_n, alpha;
};

// DiffuseMaterial Definition
class DiffuseMaterial {
  public:
    // DiffuseMaterial Type Definitions
    using BxDF = RoughDiffuseBxDF;
    using BSSRDF = void;

    // DiffuseMaterial Public Methods
    static const char *Name() { return "DiffuseMaterial"; }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static DiffuseMaterial *Create(const TextureParameterDictionary &parameters,
                                   Image *normalMap, const FileLoc *loc, Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

    DiffuseMaterial(SpectrumTexture reflectance, FloatTexture sigma,
                    FloatTexture displacement, Image *normalMap)
        : displacement(displacement),
          normalMap(normalMap),
          reflectance(reflectance),
          sigma(sigma) {}

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({sigma}, {reflectance});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, RoughDiffuseBxDF *bxdf) const {
        SampledSpectrum r = Clamp(texEval(reflectance, ctx, lambda), 0, 1);
        Float sig = Clamp(texEval(sigma, ctx), 0, 90);
        *bxdf = RoughDiffuseBxDF(r, SampledSpectrum(0), sig);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

  private:
    // DiffuseMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    SpectrumTexture reflectance;
    FloatTexture sigma;
};

// ConductorMaterial Definition
class ConductorMaterial {
  public:
    using BxDF = ConductorBxDF;
    using BSSRDF = void;

    // ConductorMaterial Public Methods
    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({uRoughness, vRoughness}, {eta, k, reflectance});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, ConductorBxDF *bxdf) const {
        // Return BSDF for _ConductorMaterial_
        Float uRough = texEval(uRoughness, ctx), vRough = texEval(vRoughness, ctx);
        if (remapRoughness) {
            uRough = TrowbridgeReitzDistribution::RoughnessToAlpha(uRough);
            vRough = TrowbridgeReitzDistribution::RoughnessToAlpha(vRough);
        }
        SampledSpectrum etas, ks;
        if (eta) {
            etas = texEval(eta, ctx, lambda);
            ks = texEval(k, ctx, lambda);
        } else {
            SampledSpectrum r = texEval(reflectance, ctx, lambda);
            etas = SampledSpectrum(1.f);
            ks = 2 * Sqrt(r) / Sqrt(ClampZero(SampledSpectrum(1) - r));
        }
        TrowbridgeReitzDistribution distrib(uRough, vRough);
        *bxdf = ConductorBxDF(distrib, etas, ks);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    ConductorMaterial(SpectrumTexture eta, SpectrumTexture k, SpectrumTexture reflectance,
                      FloatTexture uRoughness, FloatTexture vRoughness,
                      FloatTexture displacement, Image *normalMap, bool remapRoughness)
        : displacement(displacement),
          normalMap(normalMap),
          eta(eta),
          k(k),
          reflectance(reflectance),
          uRoughness(uRoughness),
          vRoughness(vRoughness),
          remapRoughness(remapRoughness) {}

    static const char *Name() { return "ConductorMaterial"; }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static ConductorMaterial *Create(const TextureParameterDictionary &parameters,
                                     Image *normalMap, const FileLoc *loc,
                                     Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // ConductorMaterial Private Data
    FloatTexture displacement;
    Image *normalMap;
    SpectrumTexture eta, k, reflectance;
    FloatTexture uRoughness, vRoughness;
    bool remapRoughness;
};

// CoatedDiffuseMaterial Definition
class CoatedDiffuseMaterial {
  public:
    using BxDF = CoatedDiffuseBxDF;
    using BSSRDF = void;
    // CoatedDiffuseMaterial Public Methods
    CoatedDiffuseMaterial(SpectrumTexture reflectance, FloatTexture uRoughness,
                          FloatTexture vRoughness, FloatTexture thickness,
                          SpectrumTexture albedo, FloatTexture g, Spectrum eta,
                          FloatTexture displacement, Image *normalMap,
                          bool remapRoughness, int maxDepth, int nSamples)
        : displacement(displacement),
          normalMap(normalMap),
          reflectance(reflectance),
          uRoughness(uRoughness),
          vRoughness(vRoughness),
          thickness(thickness),
          albedo(albedo),
          g(g),
          eta(eta),
          remapRoughness(remapRoughness),
          maxDepth(maxDepth),
          nSamples(nSamples) {}

    static const char *Name() { return "CoatedDiffuseMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({uRoughness, vRoughness, thickness, g},
                                   {reflectance, albedo});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                              SampledWavelengths &lambda, CoatedDiffuseBxDF *bxdf) const;

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static CoatedDiffuseMaterial *Create(const TextureParameterDictionary &parameters,
                                         Image *normalMap, const FileLoc *loc,
                                         Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // CoatedDiffuseMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    SpectrumTexture reflectance, albedo;
    FloatTexture uRoughness, vRoughness, thickness, g;
    Spectrum eta;
    bool remapRoughness;
    int maxDepth, nSamples;
};

// CoatedConductorMaterial Definition
class CoatedConductorMaterial {
  public:
    using BxDF = CoatedConductorBxDF;
    using BSSRDF = void;
    // CoatedConductorMaterial Public Methods
    CoatedConductorMaterial(FloatTexture interfaceURoughness,
                            FloatTexture interfaceVRoughness, FloatTexture thickness,
                            Spectrum interfaceEta, FloatTexture g, SpectrumTexture albedo,
                            FloatTexture conductorURoughness,
                            FloatTexture conductorVRoughness,
                            SpectrumTexture conductorEta, SpectrumTexture k,
                            SpectrumTexture reflectance, FloatTexture displacement,
                            Image *normalMap, bool remapRoughness, int maxDepth,
                            int nSamples)
        : displacement(displacement),
          normalMap(normalMap),
          interfaceURoughness(interfaceURoughness),
          interfaceVRoughness(interfaceVRoughness),
          thickness(thickness),
          interfaceEta(interfaceEta),
          albedo(albedo),
          g(g),
          conductorURoughness(conductorURoughness),
          conductorVRoughness(conductorVRoughness),
          conductorEta(conductorEta),
          k(k),
          reflectance(reflectance),
          remapRoughness(remapRoughness),
          maxDepth(maxDepth),
          nSamples(nSamples) {}

    static const char *Name() { return "CoatedConductorMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({interfaceURoughness, interfaceVRoughness, thickness,
                                    g, conductorURoughness, conductorVRoughness},
                                   {conductorEta, k, reflectance, albedo});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                              SampledWavelengths &lambda,
                              CoatedConductorBxDF *bxdf) const;

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static CoatedConductorMaterial *Create(const TextureParameterDictionary &parameters,
                                           Image *normalMap, const FileLoc *loc,
                                           Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // CoatedConductorMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    FloatTexture interfaceURoughness, interfaceVRoughness, thickness;
    Spectrum interfaceEta;
    FloatTexture g;
    SpectrumTexture albedo;
    FloatTexture conductorURoughness, conductorVRoughness;
    SpectrumTexture conductorEta, k, reflectance;
    bool remapRoughness;
    int maxDepth, nSamples;
};

// SubsurfaceMaterial Definition
class SubsurfaceMaterial {
  public:
    // SubsurfaceMaterial Type Definitions
    using BxDF = DielectricBxDF;
    using BSSRDF = TabulatedBSSRDF;

    // SubsurfaceMaterial Public Methods
    SubsurfaceMaterial(Float scale, SpectrumTexture sigma_a, SpectrumTexture sigma_s,
                       SpectrumTexture reflectance, SpectrumTexture mfp, Float g,
                       Float eta, FloatTexture uRoughness, FloatTexture vRoughness,
                       FloatTexture displacement, Image *normalMap, bool remapRoughness,
                       Allocator alloc)
        : displacement(displacement),
          normalMap(normalMap),
          scale(scale),
          sigma_a(sigma_a),
          sigma_s(sigma_s),
          reflectance(reflectance),
          mfp(mfp),
          uRoughness(uRoughness),
          vRoughness(vRoughness),
          eta(eta),
          remapRoughness(remapRoughness),
          table(100, 64, alloc) {
        ComputeBeamDiffusionBSSRDF(g, eta, &table);
    }

    static const char *Name() { return "SubsurfaceMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({uRoughness, vRoughness}, {sigma_a, sigma_s});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                              SampledWavelengths &lambda, DielectricBxDF *bxdf) const {
        // Initialize BSDF for _SubsurfaceMaterial_

        Float urough = texEval(uRoughness, ctx), vrough = texEval(vRoughness, ctx);
        if (remapRoughness) {
            urough = TrowbridgeReitzDistribution::RoughnessToAlpha(urough);
            vrough = TrowbridgeReitzDistribution::RoughnessToAlpha(vrough);
        }
        TrowbridgeReitzDistribution distrib(urough, vrough);

        // Initialize _bsdf_ for smooth or rough dielectric
        *bxdf = DielectricBxDF(eta, distrib);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, const MaterialEvalContext &ctx,
                                SampledWavelengths &lambda,
                                TabulatedBSSRDF *bssrdf) const {
        SampledSpectrum sig_a, sig_s;
        if (sigma_a && sigma_s) {
            // Evaluate textures for $\sigma_\roman{a}$ and $\sigma_\roman{s}$
            sig_a = ClampZero(scale * texEval(sigma_a, ctx, lambda));
            sig_s = ClampZero(scale * texEval(sigma_s, ctx, lambda));

        } else {
            // Compute _sig_a_ and _sig_s_ from reflectance and mfp
            DCHECK(reflectance && mfp);
            SampledSpectrum mfree = ClampZero(scale * texEval(mfp, ctx, lambda));
            SampledSpectrum r = Clamp(texEval(reflectance, ctx, lambda), 0, 1);
            SubsurfaceFromDiffuse(table, r, mfree, &sig_a, &sig_s);
        }
        *bssrdf = TabulatedBSSRDF(ctx.p, ctx.ns, ctx.wo, eta, sig_a, sig_s, &table);
    }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    PBRT_CPU_GPU
    static constexpr bool HasSubsurfaceScattering() { return true; }

    static SubsurfaceMaterial *Create(const TextureParameterDictionary &parameters,
                                      Image *normalMap, const FileLoc *loc,
                                      Allocator alloc);

    std::string ToString() const;

  private:
    // SubsurfaceMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    SpectrumTexture sigma_a, sigma_s, reflectance, mfp;
    Float scale, eta;
    FloatTexture uRoughness, vRoughness;
    bool remapRoughness;
    BSSRDFTable table;
};

// DiffuseTransmissionMaterial Definition
class DiffuseTransmissionMaterial {
  public:
    using BxDF = RoughDiffuseBxDF;
    using BSSRDF = void;
    // DiffuseTransmissionMaterial Public Methods
    DiffuseTransmissionMaterial(SpectrumTexture reflectance,
                                SpectrumTexture transmittance, FloatTexture sigma,
                                FloatTexture displacement, Image *normalMap, Float scale)
        : displacement(displacement),
          normalMap(normalMap),
          reflectance(reflectance),
          transmittance(transmittance),
          sigma(sigma),
          scale(scale) {}

    static const char *Name() { return "DiffuseTransmissionMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return texEval.CanEvaluate({sigma}, {reflectance, transmittance});
    }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, RoughDiffuseBxDF *bxdf) const {
        SampledSpectrum r = Clamp(scale * texEval(reflectance, ctx, lambda), 0, 1);
        SampledSpectrum t = Clamp(scale * texEval(transmittance, ctx, lambda), 0, 1);
        Float s = texEval(sigma, ctx);
        *bxdf = RoughDiffuseBxDF(r, t, s);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static DiffuseTransmissionMaterial *Create(
        const TextureParameterDictionary &parameters, Image *normalMap,
        const FileLoc *loc, Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // DiffuseTransmissionMaterial Private Data
    FloatTexture displacement;
    Image *normalMap;
    SpectrumTexture reflectance, transmittance;
    FloatTexture sigma;
    Float scale;
};

// MeasuredMaterial Definition
class MeasuredMaterial {
  public:
    using BxDF = MeasuredBxDF;
    using BSSRDF = void;
    // MeasuredMaterial Public Methods
    template <typename TextureEvaluator>
    PBRT_CPU_GPU BSDF GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda, MeasuredBxDF *bxdf) const {
        *bxdf = MeasuredBxDF(brdf, lambda);
        return BSDF(ctx.ns, ctx.dpdus, bxdf);
    }

    MeasuredMaterial(const std::string &filename, FloatTexture displacement,
                     Image *normalMap, Allocator alloc);

    static const char *Name() { return "MeasuredMaterial"; }

    template <typename TextureEvaluator>
    PBRT_CPU_GPU bool CanEvaluateTextures(TextureEvaluator texEval) const {
        return true;
    }

    PBRT_CPU_GPU
    FloatTexture GetDisplacement() const { return displacement; }
    PBRT_CPU_GPU
    const Image *GetNormalMap() const { return normalMap; }

    static MeasuredMaterial *Create(const TextureParameterDictionary &parameters,
                                    Image *normalMap, const FileLoc *loc,
                                    Allocator alloc);

    template <typename TextureEvaluator>
    PBRT_CPU_GPU void GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                SampledWavelengths &lambda, void *) const {}

    PBRT_CPU_GPU static constexpr bool HasSubsurfaceScattering() { return false; }

    std::string ToString() const;

  private:
    // MeasuredMaterial Private Members
    FloatTexture displacement;
    Image *normalMap;
    const MeasuredBRDF *brdf;
};

// Material Inline Method Definitions
template <typename TextureEvaluator>
inline BSDF Material::GetBSDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                              SampledWavelengths &lambda,
                              ScratchBuffer &scratchBuffer) const {
    // Define _getBSDF_ lamba function for _Material::GetBSDF()_
    auto getBSDF = [&](auto mtl) -> BSDF {
        using Material = typename std::remove_reference<decltype(*mtl)>::type;
        using BxDF = typename Material::BxDF;
        BxDF *bxdf = (BxDF *)scratchBuffer.Alloc(sizeof(BxDF), alignof(BxDF));
        return mtl->GetBSDF(texEval, ctx, lambda, bxdf);
    };

    return Dispatch(getBSDF);
}

template <typename TextureEvaluator>
inline bool Material::CanEvaluateTextures(TextureEvaluator texEval) const {
    auto eval = [&](auto ptr) { return ptr->CanEvaluateTextures(texEval); };
    return Dispatch(eval);
}

template <typename TextureEvaluator>
inline BSSRDF Material::GetBSSRDF(TextureEvaluator texEval, MaterialEvalContext ctx,
                                  SampledWavelengths &lambda,
                                  ScratchBuffer &scratchBuffer) const {
    auto get = [&](auto ptr) -> BSSRDF {
        using Material = typename std::remove_reference<decltype(*ptr)>::type;
        using BSSRDF = typename Material::BSSRDF;
        if constexpr (std::is_same_v<BSSRDF, void>)
            return nullptr;
        else {
            BSSRDF *bssrdf =
                (BSSRDF *)scratchBuffer.Alloc(sizeof(BSSRDF), alignof(BSSRDF));
            ptr->GetBSSRDF(texEval, ctx, lambda, bssrdf);
            return bssrdf;
        }
    };
    return Dispatch(get);
}

inline bool Material::HasSubsurfaceScattering() const {
    auto has = [&](auto ptr) { return ptr->HasSubsurfaceScattering(); };
    return Dispatch(has);
}

inline FloatTexture Material::GetDisplacement() const {
    auto disp = [&](auto ptr) { return ptr->GetDisplacement(); };
    return Dispatch(disp);
}

inline const Image *Material::GetNormalMap() const {
    auto nmap = [&](auto ptr) { return ptr->GetNormalMap(); };
    return Dispatch(nmap);
}

}  // namespace pbrt

#endif  // PBRT_MATERIALS_H
