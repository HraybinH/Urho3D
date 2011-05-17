//
// Urho3D Engine
// Copyright (c) 2008-2011 Lasse ��rni
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Context.h"
#include "FileSystem.h"
#include "Graphics.h"
#include "Log.h"
#include "Material.h"
#include "Matrix4x3.h"
#include "Profiler.h"
#include "ResourceCache.h"
#include "StringUtils.h"
#include "Technique.h"
#include "Texture2D.h"
#include "TextureCube.h"
#include "XMLFile.h"

#include "DebugNew.h"

static const String textureUnitNames[] =
{
    "diffuse",
    "normal",
    "specular",
    "detail",
    "environment",
    "emissive",
    "lightramp", // Not defined by materials
    "lightspot" // Not defined by materials
};

static const String cullModeNames[] =
{
    "none",
    "ccw",
    "cw"
};

TechniqueEntry::TechniqueEntry() :
    qualityLevel_(0),
    lodDistance_(0.0f)
{
}

TechniqueEntry::TechniqueEntry(Technique* technique, unsigned qualityLevel, float lodDistance) :
    technique_(technique),
    qualityLevel_(qualityLevel),
    lodDistance_(lodDistance)
{
}

TechniqueEntry::~TechniqueEntry()
{
}

OBJECTTYPESTATIC(Material);

Material::Material(Context* context) :
    Resource(context),
    cullMode_(CULL_CCW),
    shadowCullMode_(CULL_CCW),
    auxViewFrameNumber_(0),
    occlusion_(true)
{
    SetNumTechniques(1);
    textures_.Resize(MAX_MATERIAL_TEXTURE_UNITS);
    
    // Setup often used default parameters
    vsParameters_[VSP_UOFFSET] = Vector4(1.0f, 0.0f, 0.0f, 0.0f);
    vsParameters_[VSP_VOFFSET] = Vector4(0.0f, 1.0f, 0.0f, 0.0f);
    psParameters_[PSP_MATDIFFCOLOR] = Vector4::UNITY;
    psParameters_[PSP_MATEMISSIVECOLOR] = Vector4::ZERO;
    psParameters_[PSP_MATSPECPROPERTIES] = Vector4::ZERO;
}

Material::~Material()
{
}

void Material::RegisterObject(Context* context)
{
    context->RegisterFactory<Material>();
}

bool Material::Load(Deserializer& source)
{
    PROFILE(LoadMaterial);
    
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    Graphics* graphics = GetSubsystem<Graphics>();
    if ((!cache) || (!graphics))
        return false;
    
    SharedPtr<XMLFile> xml(new XMLFile(context_));
    if (!xml->Load(source))
        return false;
    
    XMLElement rootElem = xml->GetRootElement();
    
    XMLElement techniqueElem = rootElem.GetChildElement("technique");
    techniques_.Clear();
    while (techniqueElem)
    {
        Technique* technique = cache->GetResource<Technique>(techniqueElem.GetString("name"));
        if (technique)
        {
            TechniqueEntry newTechnique;
            newTechnique.technique_ = technique;
            if (techniqueElem.HasAttribute("quality"))
                newTechnique.qualityLevel_ = techniqueElem.GetInt("quality");
            if (techniqueElem.HasAttribute("loddistance"))
                newTechnique.lodDistance_ = techniqueElem.GetFloat("loddistance");
            techniques_.Push(newTechnique);
        }
        techniqueElem = techniqueElem.GetNextElement("technique");
    }
    
    XMLElement textureElem = rootElem.GetChildElement("texture");
    while (textureElem)
    {
        TextureUnit unit = TU_DIFFUSE;
        if (textureElem.HasAttribute("unit"))
        {
            String unitName = textureElem.GetStringLower("unit");
            unit = (TextureUnit)GetStringListIndex(unitName, textureUnitNames, MAX_MATERIAL_TEXTURE_UNITS,
                MAX_MATERIAL_TEXTURE_UNITS);
            if (unitName == "diff")
                unit = TU_DIFFUSE;
            if (unitName == "norm")
                unit = TU_NORMAL;
            if (unitName == "spec")
                unit = TU_SPECULAR;
            if (unitName == "env")
                unit = TU_ENVIRONMENT;
            if (unit == MAX_MATERIAL_TEXTURE_UNITS)
                LOGERROR("Unknown texture unit " + unitName);
        }
        if (unit != MAX_MATERIAL_TEXTURE_UNITS)
        {
            String name = textureElem.GetString("name");
            // Detect cube maps by file extension: they are defined by an XML file
            if (GetExtension(name) == ".xml")
                SetTexture(unit, cache->GetResource<TextureCube>(name));
            else
                SetTexture(unit, cache->GetResource<Texture2D>(name));
        }
        textureElem = textureElem.GetNextElement("texture");
    }
    
    XMLElement parameterElem = rootElem.GetChildElement("parameter");
    while (parameterElem)
    {
        String name = parameterElem.GetString("name");
        Vector4 value = parameterElem.GetVector("value");
        VSParameter vsParam = graphics->GetVSParameter(name);
        if (vsParam != MAX_VS_PARAMETERS)
            SetVertexShaderParameter(vsParam, value);
        else
        {
            PSParameter psParam = graphics->GetPSParameter(name);
            if (psParam != MAX_PS_PARAMETERS)
                SetPixelShaderParameter(psParam, value);
            else
                LOGERROR("Unknown shader parameter " + name);
        }
        
        parameterElem = parameterElem.GetNextElement("parameter");
    }
    
    XMLElement cullElem = rootElem.GetChildElement("cull");
    if (cullElem)
        SetCullMode((CullMode)GetStringListIndex(cullElem.GetString("value"), cullModeNames, MAX_CULLMODES, CULL_CCW));
    
    XMLElement shadowCullElem = rootElem.GetChildElement("shadowcull");
    if (shadowCullElem)
        SetShadowCullMode((CullMode)GetStringListIndex(shadowCullElem.GetString("value"), cullModeNames, MAX_CULLMODES, CULL_CCW));
    
    // Calculate memory use
    unsigned memoryUse = 0;
    memoryUse += sizeof(Material);
    memoryUse += techniques_.Size() * sizeof(TechniqueEntry);
    memoryUse += textures_.Size() * sizeof(SharedPtr<Texture>);
    memoryUse += vsParameters_.Size() * (sizeof(VSParameter) + sizeof(Vector4));
    memoryUse += psParameters_.Size() * (sizeof(PSParameter) + sizeof(Vector4));
    
    SetMemoryUse(memoryUse);
    Update();
    return true;
}

bool Material::Save(Serializer& dest)
{
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;
    
    SharedPtr<XMLFile> xml(new XMLFile(context_));
    XMLElement materialElem = xml->CreateRootElement("material");
    
    // Write techniques
    for (unsigned i = 0; i < techniques_.Size(); ++i)
    {
        TechniqueEntry& entry = techniques_[i];
        if (!entry.technique_)
            continue;
        
        XMLElement techniqueElem = materialElem.CreateChildElement("technique");
        techniqueElem.SetString("name", entry.technique_->GetName());
        techniqueElem.SetInt("quality", entry.qualityLevel_);
        techniqueElem.SetFloat("loddistance", entry.lodDistance_);
    }
    
    // Write texture units
    for (unsigned j = 0; j < MAX_MATERIAL_TEXTURE_UNITS; ++j)
    {
        Texture* texture = GetTexture((TextureUnit)j);
        if (texture)
        {
            XMLElement textureElem = materialElem.CreateChildElement("texture");
            textureElem.SetString("unit", textureUnitNames[j]);
            textureElem.SetString("name", texture->GetName());
        }
    }
    
    // Write shader parameters
    for (Map<VSParameter, Vector4>::ConstIterator j = vsParameters_.Begin(); j != vsParameters_.End(); ++j)
    {
        XMLElement parameterElem = materialElem.CreateChildElement("parameter");
        parameterElem.SetString("name", graphics->GetVSParameterName(j->first_));
        parameterElem.SetVector4("value", j->second_);
    }
    for (Map<PSParameter, Vector4>::ConstIterator j = psParameters_.Begin(); j != psParameters_.End(); ++j)
    {
        XMLElement parameterElem = materialElem.CreateChildElement("parameter");
        parameterElem.SetString("name", graphics->GetPSParameterName(j->first_));
        parameterElem.SetVector4("value", j->second_);
    }
    
    return xml->Save(dest);
}

void Material::SetNumTechniques(unsigned num)
{
    if (!num)
        return;
    
    techniques_.Resize(num);
}

void Material::SetTechnique(unsigned index, Technique* technique, unsigned qualityLevel, float lodDistance)
{
    if (index >= techniques_.Size())
        return;
    
    techniques_[index] = TechniqueEntry(technique, qualityLevel, lodDistance);
    Update();
}

void Material::SetVertexShaderParameter(VSParameter parameter, const Vector4& value)
{
    vsParameters_[parameter] = value;
}

void Material::SetPixelShaderParameter(PSParameter parameter, const Vector4& value)
{
    psParameters_[parameter] = value;
}

void Material::SetTexture(TextureUnit unit, Texture* texture)
{
    if (unit >= MAX_MATERIAL_TEXTURE_UNITS)
        return;
    
    textures_[unit] = texture;
}

void Material::SetUVTransform(const Vector2& offset, float rotation, const Vector2& repeat)
{
    Matrix4x3 transform(Matrix4x3::IDENTITY);
    transform.m00_ = repeat.x_;
    transform.m11_ = repeat.y_;
    transform.m03_ = -0.5f * transform.m00_ + 0.5f;
    transform.m13_ = -0.5f * transform.m11_ + 0.5f;
    
    Matrix4x3 rotationMatrix(Matrix4x3::IDENTITY);
    float angleRad = rotation * M_DEGTORAD;
    rotationMatrix.m00_ = cosf(angleRad);
    rotationMatrix.m01_ = sinf(angleRad);
    rotationMatrix.m10_ = -rotationMatrix.m01_;
    rotationMatrix.m11_ = rotationMatrix.m00_;
    rotationMatrix.m03_ = 0.5f - 0.5f * (rotationMatrix.m00_ + rotationMatrix.m01_);
    rotationMatrix.m13_ = 0.5f - 0.5f * (rotationMatrix.m10_ + rotationMatrix.m11_);
    
    transform = rotationMatrix * transform;
    
    Matrix4x3 offsetMatrix = Matrix4x3::IDENTITY;
    offsetMatrix.m03_ = offset.x_;
    offsetMatrix.m13_ = offset.y_;
    
    transform = offsetMatrix * transform;
    
    Vector4& uOffset = vsParameters_[VSP_UOFFSET];
    Vector4& vOffset = vsParameters_[VSP_VOFFSET];
    uOffset.x_ = transform.m00_;
    uOffset.y_ = transform.m01_;
    uOffset.w_ = transform.m03_;
    vOffset.x_ = transform.m10_;
    vOffset.y_ = transform.m11_;
    vOffset.w_ = transform.m13_;
}

void Material::SetUVTransform(const Vector2& offset, float rotation, float repeat)
{
    SetUVTransform(offset, rotation, Vector2(repeat, repeat));
}

void Material::SetCullMode(CullMode mode)
{
    cullMode_ = mode;
}

void Material::SetShadowCullMode(CullMode mode)
{
    shadowCullMode_ = mode;
}

void Material::ReleaseShaders()
{
    for (unsigned i = 0; i < techniques_.Size(); ++i)
    {
        Technique* technique = techniques_[i].technique_;
        if (technique)
            technique->ReleaseShaders();
    }
}

SharedPtr<Material> Material::Clone(const String& cloneName) const
{
    SharedPtr<Material> ret(new Material(context_));
    
    ret->SetName(cloneName);
    ret->techniques_ = techniques_;
    ret->vsParameters_ = vsParameters_;
    ret->psParameters_ = psParameters_;
    ret->textures_ = textures_;
    ret->occlusion_ = occlusion_;
    ret->cullMode_ = cullMode_;
    ret->shadowCullMode_ = shadowCullMode_;
    
    return ret;
}

void Material::MarkForAuxView(unsigned frameNumber)
{
    auxViewFrameNumber_ = frameNumber;
}

const TechniqueEntry& Material::GetTechniqueEntry(unsigned index) const
{
    TechniqueEntry noEntry;
    return index < techniques_.Size() ? techniques_[index] : noEntry;
}

Technique* Material::GetTechnique(unsigned index) const
{
    return index < techniques_.Size() ? techniques_[index].technique_ : (Technique*)0;
}

Pass* Material::GetPass(unsigned index, PassType pass) const
{
    Technique* technique = index < techniques_.Size() ? techniques_[index].technique_ : (Technique*)0;
    return technique ? technique->GetPass(pass) : 0;
}

Texture* Material::GetTexture(TextureUnit unit) const
{
    return (unsigned)unit < textures_.Size() ? textures_[unit] : (Texture*)0;
}

const String& Material::GetTextureUnitName(TextureUnit unit)
{
    return textureUnitNames[unit];
}

void Material::Update()
{
    // Determine occlusion by checking the first pass of each technique
    occlusion_ = false;
    for (unsigned i = 0; i < techniques_.Size(); ++i)
    {
        Technique* technique = techniques_[i].technique_;
        if (!technique)
            continue;
        
        const Map<PassType, Pass>& passes = technique->GetPasses();
        if (!passes.Empty())
        {
            // If pass writes depth, enable occlusion
            const Pass& pass = passes.Begin()->second_;
            if (pass.GetDepthWrite())
            {
                occlusion_ = true;
                break;
            }
        }
    }
}
