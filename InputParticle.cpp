#include "InputParticle.h"

#include <maya/MDoubleArray.h>
#include <maya/MFnAttribute.h>
#include <maya/MFnParticleSystem.h>
#include <maya/MGlobal.h>
#include <maya/MMatrix.h>
#include <maya/MPlug.h>
#include <maya/MPlugArray.h>
#include <maya/MStringArray.h>
#include <maya/MVectorArray.h>

#include "hapiutil.h"
#include "util.h"

InputParticle::InputParticle() : Input()
{
    Util::PythonInterpreterLock pythonInterpreterLock;

    HAPI_NodeId nodeId;
    CHECK_HAPI(HoudiniApi::CreateInputNode(Util::theHAPISession.get(), -1, &nodeId, NULL));
    if (!Util::statusCheckLoop())
    {
        DISPLAY_ERROR(MString("Unexpected error when creating input particle."));
    }

    HAPI_NodeInfo nodeInfo;
    HoudiniApi::GetNodeInfo(Util::theHAPISession.get(), nodeId, &nodeInfo);

    setTransformNodeId(nodeInfo.parentId);
    setGeometryNodeId(nodeId);
}

InputParticle::~InputParticle()
{
    if (!Util::theHAPISession.get())
        return;
    CHECK_HAPI(HoudiniApi::DeleteNode(Util::theHAPISession.get(), geometryNodeId()));
}

Input::AssetInputType
InputParticle::assetInputType() const
{
    return Input::AssetInputType_Particle;
}

void
InputParticle::setAttributePointData(const char *attributeName,
                                     HAPI_StorageType storage,
                                     int count,
                                     int tupleSize,
                                     void *data)
{
    HAPI_AttributeInfo attributeInfo;
    attributeInfo.exists    = true;
    attributeInfo.owner     = HAPI_ATTROWNER_POINT;
    attributeInfo.storage   = storage;
    attributeInfo.count     = count;
    attributeInfo.tupleSize = tupleSize;

    HoudiniApi::AddAttribute(Util::theHAPISession.get(), geometryNodeId(), 0,
                      attributeName, &attributeInfo);

    switch (storage)
    {
    case HAPI_STORAGETYPE_FLOAT:
        HoudiniApi::SetAttributeFloatData(Util::theHAPISession.get(), geometryNodeId(),
                                   0, attributeName, &attributeInfo,
                                   static_cast<float *>(data), 0, count);
        break;
    case HAPI_STORAGETYPE_INT:
        HoudiniApi::SetAttributeIntData(Util::theHAPISession.get(), geometryNodeId(),
                                 0, attributeName, &attributeInfo,
                                 static_cast<int *>(data), 0, count);
        break;
    default:
        break;
    }
}

void
InputParticle::setInputGeo(MDataBlock &dataBlock, const MPlug &plug)
{
    MStatus status;

    // get particle node
    MObject particleObj;
    {
        MPlug srcPlug = Util::plugSource(plug);
        if (!srcPlug.isNull())
        {
            particleObj = srcPlug.node();
        }
    }

    if (particleObj.isNull())
    {
        return;
    }

    MFnParticleSystem particleFn(particleObj);

    // get original particle node
    MObject originalParticleObj;
    // status parameter is needed due to a bug in Maya API
    if (particleFn.isDeformedParticleShape(&status))
    {
        originalParticleObj = particleFn.originalParticleShape(&status);
    }
    else
    {
        originalParticleObj = particleObj;
    }

    MFnParticleSystem originalParticleFn(originalParticleObj);

    // set up part info
    HAPI_PartInfo partInfo;
    HoudiniApi::PartInfo_Init(&partInfo);
    partInfo.id          = 0;
    partInfo.faceCount   = 0;
    partInfo.vertexCount = 0;
    partInfo.pointCount  = particleFn.count();

    HoudiniApi::SetPartInfo(
        Util::theHAPISession.get(), geometryNodeId(), 0, &partInfo);

    // set per-particle attributes
    {
        // id
        {
            MIntArray ids;
            // Must get the IDs from the original particle node. Maya will
            // crash if we try to get the IDs from the deformed particle node.
            originalParticleFn.particleIds(ids);

            CHECK_HAPI(
                hapiSetPointAttribute(geometryNodeId(), 0, 1, "id", ids));
        }

        // vector attributes
        {
            MVectorArray vectorArray;

            bool doPreserveAttrScale = false;

            // query the original particle for names of the per-particle
            // attributes
            MString getAttributesCommand = "particle -q -perParticleVector ";
            getAttributesCommand += originalParticleFn.fullPathName();

            // get the per-particle attribute names
            MStringArray attributeNames;
            MGlobal::executeCommand(getAttributesCommand, attributeNames);

            for (unsigned int ai = 0; ai < attributeNames.length(); ai++)
            {
                const MString attributeName = attributeNames[ai];
                MObject attributeObj =
                    originalParticleFn.attribute(attributeName);
                if (attributeObj.isNull())
                {
                    continue;
                }

                // mimics "listAttr -v -w" from AEokayAttr
                MFnAttribute attributeFn(attributeObj);
                if (!(!attributeFn.isHidden() && attributeFn.isWritable()) &&
                    (attributeName != "age" &&
                     attributeName != "finalLifespanPP"))
                {
                    continue;
                }

                // get the per-particle data
                if (attributeName == "position")
                {
                    // Need to use position() so that we get the right
                    // positions in the case of deformed particles.
                    particleFn.position(vectorArray);
                }
                else
                {
                    // Maya will automatically use the original
                    // particle node in the case of deformed particles.
                    particleFn.getPerParticleAttribute(
                        attributeName, vectorArray);
                }

                // When particle node is initially loaded from a scene file, if
                // the attribute is driven by expressions, then
                // MFnParticleSystem doesn't initially seem to have data.
                if (partInfo.pointCount != (int)vectorArray.length())
                {
                    vectorArray.setLength(partInfo.pointCount);
                }

                // map the attribute name
                const char *mappedAttributeName = attributeName.asChar();
                if (strcmp(mappedAttributeName, "position") == 0)
                {
                    mappedAttributeName = "P";
                    doPreserveAttrScale = true;
                }
                else if (strcmp(mappedAttributeName, "velocity") == 0)
                {
                    mappedAttributeName = "v";
                    doPreserveAttrScale = true;
                }
                else if (strcmp(mappedAttributeName, "acceleration") == 0)
                {
                    mappedAttributeName = "force";
                    doPreserveAttrScale = true;
                }
                else if (strcmp(mappedAttributeName, "rgbPP") == 0)
                {
                    mappedAttributeName = "Cd";
                }

                if (myPreserveScale && doPreserveAttrScale)
                {
                    for (unsigned int i = 0; i < vectorArray.length(); i++)
                        vectorArray[i] *= 0.01;
                }

                CHECK_HAPI(hapiSetPointAttribute(
                    geometryNodeId(), 0, 3, mappedAttributeName,
                    Util::reshapeArray<3, std::vector<double>>(vectorArray)));
            }
        }

        // double attributes
        {
            MDoubleArray doubleArray;

            bool doPreserveAttrScale = false;

            // query the original particle for names of the per-particle
            // attributes
            MString getAttributesCommand = "particle -q -perParticleDouble ";
            getAttributesCommand += originalParticleFn.fullPathName();

            // get the per-particle attribute names
            MStringArray attributeNames;
            MGlobal::executeCommand(getAttributesCommand, attributeNames);

            // explicitly include some special per-particle attributes that
            // aren't returned by the MEL command
            attributeNames.append("age");

            for (unsigned int ai = 0; ai < attributeNames.length(); ai++)
            {
                const MString attributeName = attributeNames[ai];

                MObject attributeObj =
                    originalParticleFn.attribute(attributeName);
                if (attributeObj.isNull())
                {
                    continue;
                }

                // mimics "listAttr -v -w" from AEokayAttr
                MFnAttribute attributeFn(attributeObj);
                if (!(!attributeFn.isHidden() && attributeFn.isWritable()) &&
                    (attributeName != "age" &&
                     attributeName != "finalLifespanPP"))
                {
                    continue;
                }

                // get the per-particle data
                // Maya will automatically use the original
                // particle node in the case of deformed particles.
                particleFn.getPerParticleAttribute(attributeName, doubleArray);

                // When particle node is initially loaded from a scene file, if
                // the attribute is driven by expressions, then
                // MFnParticleSystem doesn't initially seem to have data.
                if (partInfo.pointCount != (int)doubleArray.length())
                {
                    doubleArray.setLength(partInfo.pointCount);
                }

                // map the parameter name
                const char *mappedAttributeName = attributeName.asChar();
                if (strcmp(mappedAttributeName, "opacityPP") == 0)
                {
                    mappedAttributeName = "Alpha";
                }
                else if (strcmp(mappedAttributeName, "radiusPP") == 0)
                {
                    mappedAttributeName = "pscale";
                    doPreserveAttrScale = true;
                }
                else if (strcmp(mappedAttributeName, "finalLifespanPP") == 0)
                {
                    mappedAttributeName = "life";
                }

                if (myPreserveScale && doPreserveAttrScale)
                {
                    for (unsigned int i = 0; i < doubleArray.length(); i++)
                        doubleArray[i] *= 0.01;
                }

                CHECK_HAPI(hapiSetPointAttribute(
                    geometryNodeId(), 0, 1, mappedAttributeName, doubleArray));
            }
        }
    }

    setInputName(HAPI_ATTROWNER_POINT, partInfo.pointCount, plug);

    // Commit it
    HoudiniApi::CommitGeo(Util::theHAPISession.get(), geometryNodeId());
}

