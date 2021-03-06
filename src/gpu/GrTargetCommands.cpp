/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrTargetCommands.h"

#include "GrColor.h"
#include "GrDefaultGeoProcFactory.h"
#include "GrInOrderDrawBuffer.h"
#include "GrTemplates.h"
#include "SkPoint.h"

static bool path_fill_type_is_winding(const GrStencilSettings& pathStencilSettings) {
    static const GrStencilSettings::Face pathFace = GrStencilSettings::kFront_Face;
    bool isWinding = kInvert_StencilOp != pathStencilSettings.passOp(pathFace);
    if (isWinding) {
        // Double check that it is in fact winding.
        SkASSERT(kIncClamp_StencilOp == pathStencilSettings.passOp(pathFace));
        SkASSERT(kIncClamp_StencilOp == pathStencilSettings.failOp(pathFace));
        SkASSERT(0x1 != pathStencilSettings.writeMask(pathFace));
        SkASSERT(!pathStencilSettings.isTwoSided());
    }
    return isWinding;
}

GrTargetCommands::Cmd* GrTargetCommands::recordDrawBatch(
                                                  GrInOrderDrawBuffer* iodb,
                                                  GrBatch* batch,
                                                  const GrDrawTarget::PipelineInfo& pipelineInfo) {
    if (!this->setupPipelineAndShouldDraw(iodb, batch, pipelineInfo)) {
        return NULL;
    }

    // Check if there is a Batch Draw we can batch with
    if (Cmd::kDrawBatch_CmdType != fCmdBuffer.back().type() || !fDrawBatch) {
        fDrawBatch = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, DrawBatch, (batch, &fBatchTarget));
        return fDrawBatch;
    }

    SkASSERT(&fCmdBuffer.back() == fDrawBatch);
    if (!fDrawBatch->fBatch->combineIfPossible(batch)) {
        fDrawBatch = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, DrawBatch, (batch, &fBatchTarget));
    }

    return fDrawBatch;
}

GrTargetCommands::Cmd* GrTargetCommands::recordStencilPath(
                                                        GrInOrderDrawBuffer* iodb,
                                                        const GrPipelineBuilder& pipelineBuilder,
                                                        const GrPathProcessor* pathProc,
                                                        const GrPath* path,
                                                        const GrScissorState& scissorState,
                                                        const GrStencilSettings& stencilSettings) {
    StencilPath* sp = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, StencilPath,
                                               (path, pipelineBuilder.getRenderTarget()));

    sp->fScissor = scissorState;
    sp->fUseHWAA = pipelineBuilder.isHWAntialias();
    sp->fViewMatrix = pathProc->viewMatrix();
    sp->fStencil = stencilSettings;
    return sp;
}

GrTargetCommands::Cmd* GrTargetCommands::recordDrawPath(
                                                  GrInOrderDrawBuffer* iodb,
                                                  const GrPathProcessor* pathProc,
                                                  const GrPath* path,
                                                  const GrStencilSettings& stencilSettings,
                                                  const GrDrawTarget::PipelineInfo& pipelineInfo) {
    // TODO: Only compare the subset of GrPipelineBuilder relevant to path covering?
    if (!this->setupPipelineAndShouldDraw(iodb, pathProc, pipelineInfo)) {
        return NULL;
    }
    DrawPath* dp = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, DrawPath, (path));
    dp->fStencilSettings = stencilSettings;
    return dp;
}

GrTargetCommands::Cmd* GrTargetCommands::recordDrawPaths(
                                                  GrInOrderDrawBuffer* iodb,
                                                  const GrPathProcessor* pathProc,
                                                  const GrPathRange* pathRange,
                                                  const void* indexValues,
                                                  GrDrawTarget::PathIndexType indexType,
                                                  const float transformValues[],
                                                  GrDrawTarget::PathTransformType transformType,
                                                  int count,
                                                  const GrStencilSettings& stencilSettings,
                                                  const GrDrawTarget::PipelineInfo& pipelineInfo) {
    SkASSERT(pathRange);
    SkASSERT(indexValues);
    SkASSERT(transformValues);

    if (!this->setupPipelineAndShouldDraw(iodb, pathProc, pipelineInfo)) {
        return NULL;
    }

    char* savedIndices;
    float* savedTransforms;
    
    iodb->appendIndicesAndTransforms(indexValues, indexType,
                                     transformValues, transformType,
                                     count, &savedIndices, &savedTransforms);

    if (Cmd::kDrawPaths_CmdType == fCmdBuffer.back().type()) {
        // The previous command was also DrawPaths. Try to collapse this call into the one
        // before. Note that stenciling all the paths at once, then covering, may not be
        // equivalent to two separate draw calls if there is overlap. Blending won't work,
        // and the combined calls may also cancel each other's winding numbers in some
        // places. For now the winding numbers are only an issue if the fill is even/odd,
        // because DrawPaths is currently only used for glyphs, and glyphs in the same
        // font tend to all wind in the same direction.
        DrawPaths* previous = static_cast<DrawPaths*>(&fCmdBuffer.back());
        if (pathRange == previous->pathRange() &&
            indexType == previous->fIndexType &&
            transformType == previous->fTransformType &&
            stencilSettings == previous->fStencilSettings &&
            path_fill_type_is_winding(stencilSettings) &&
            !pipelineInfo.willBlendWithDst(pathProc)) {
                const int indexBytes = GrPathRange::PathIndexSizeInBytes(indexType);
                const int xformSize = GrPathRendering::PathTransformSize(transformType);
                if (&previous->fIndices[previous->fCount*indexBytes] == savedIndices &&
                    (0 == xformSize ||
                     &previous->fTransforms[previous->fCount*xformSize] == savedTransforms)) {
                    // Fold this DrawPaths call into the one previous.
                    previous->fCount += count;
                    return NULL;
                }
        }
    }

    DrawPaths* dp = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, DrawPaths, (pathRange));
    dp->fIndices = savedIndices;
    dp->fIndexType = indexType;
    dp->fTransforms = savedTransforms;
    dp->fTransformType = transformType;
    dp->fCount = count;
    dp->fStencilSettings = stencilSettings;
    return dp;
}

GrTargetCommands::Cmd* GrTargetCommands::recordClear(GrInOrderDrawBuffer* iodb,
                                                     const SkIRect* rect, 
                                                     GrColor color,
                                                     bool canIgnoreRect,
                                                     GrRenderTarget* renderTarget) {
    SkASSERT(renderTarget);

    SkIRect r;
    if (NULL == rect) {
        // We could do something smart and remove previous draws and clears to
        // the current render target. If we get that smart we have to make sure
        // those draws aren't read before this clear (render-to-texture).
        r.setLTRB(0, 0, renderTarget->width(), renderTarget->height());
        rect = &r;
    }
    Clear* clr = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, Clear, (renderTarget));
    GrColorIsPMAssert(color);
    clr->fColor = color;
    clr->fRect = *rect;
    clr->fCanIgnoreRect = canIgnoreRect;
    return clr;
}

GrTargetCommands::Cmd* GrTargetCommands::recordClearStencilClip(GrInOrderDrawBuffer* iodb,
                                                                const SkIRect& rect,
                                                                bool insideClip,
                                                                GrRenderTarget* renderTarget) {
    SkASSERT(renderTarget);

    ClearStencilClip* clr = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, ClearStencilClip, (renderTarget));
    clr->fRect = rect;
    clr->fInsideClip = insideClip;
    return clr;
}

GrTargetCommands::Cmd* GrTargetCommands::recordDiscard(GrInOrderDrawBuffer* iodb,
                                                       GrRenderTarget* renderTarget) {
    SkASSERT(renderTarget);

    Clear* clr = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, Clear, (renderTarget));
    clr->fColor = GrColor_ILLEGAL;
    return clr;
}

void GrTargetCommands::reset() {
    fCmdBuffer.reset();
    fPrevState = NULL;
    fDrawBatch = NULL;
}

void GrTargetCommands::flush(GrInOrderDrawBuffer* iodb) {
    if (fCmdBuffer.empty()) {
        return;
    }

    // Updated every time we find a set state cmd to reflect the current state in the playback
    // stream.
    SetState* currentState = NULL;

    GrGpu* gpu = iodb->getGpu();

#ifdef USE_BITMAP_TEXTBLOBS
    // Loop over all batches and generate geometry
    CmdBuffer::Iter genIter(fCmdBuffer);
    while (genIter.next()) {
        if (Cmd::kDrawBatch_CmdType == genIter->type()) {
            DrawBatch* db = reinterpret_cast<DrawBatch*>(genIter.get());
            fBatchTarget.resetNumberOfDraws();
            db->execute(NULL, currentState);
            db->fBatch->setNumberOfDraws(fBatchTarget.numberOfDraws());
        } else if (Cmd::kSetState_CmdType == genIter->type()) {
            SetState* ss = reinterpret_cast<SetState*>(genIter.get());

            ss->execute(gpu, currentState);
            currentState = ss;
        }
    }
#endif

    iodb->getVertexAllocPool()->unmap();
    iodb->getIndexAllocPool()->unmap();
    fBatchTarget.preFlush();

    CmdBuffer::Iter iter(fCmdBuffer);

    while (iter.next()) {
        GrGpuTraceMarker newMarker("", -1);
        SkString traceString;
        if (iter->isTraced()) {
            traceString = iodb->getCmdString(iter->markerID());
            newMarker.fMarker = traceString.c_str();
            gpu->addGpuTraceMarker(&newMarker);
        }

        // TODO temporary hack
        if (Cmd::kDrawBatch_CmdType == iter->type()) {
            DrawBatch* db = reinterpret_cast<DrawBatch*>(iter.get());
            fBatchTarget.flushNext(db->fBatch->numberOfDraws());

            if (iter->isTraced()) {
                gpu->removeGpuTraceMarker(&newMarker);
            }
            continue;
        }

        if (Cmd::kSetState_CmdType == iter->type()) {
#ifndef USE_BITMAP_TEXTBLOBS
            SetState* ss = reinterpret_cast<SetState*>(iter.get());

            ss->execute(gpu, currentState);
            currentState = ss;
#else
            // TODO this is just until NVPR is in batch
            SetState* ss = reinterpret_cast<SetState*>(iter.get());

            if (ss->fPrimitiveProcessor) {
                ss->execute(gpu, currentState);
            }
            currentState = ss;
#endif

        } else {
            iter->execute(gpu, currentState);
        }

        if (iter->isTraced()) {
            gpu->removeGpuTraceMarker(&newMarker);
        }
    }

    // TODO see copious notes about hack
    fBatchTarget.postFlush();
}

void GrTargetCommands::Draw::execute(GrGpu* gpu, const SetState* state) {
    SkASSERT(state);
    DrawArgs args(state->fPrimitiveProcessor.get(), state->getPipeline(), &state->fDesc,
                  &state->fBatchTracker);
    gpu->draw(args, fInfo);
}

void GrTargetCommands::StencilPath::execute(GrGpu* gpu, const SetState*) {
    GrGpu::StencilPathState state;
    state.fRenderTarget = fRenderTarget.get();
    state.fScissor = &fScissor;
    state.fStencil = &fStencil;
    state.fUseHWAA = fUseHWAA;
    state.fViewMatrix = &fViewMatrix;

    gpu->stencilPath(this->path(), state);
}

void GrTargetCommands::DrawPath::execute(GrGpu* gpu, const SetState* state) {
    SkASSERT(state);
    DrawArgs args(state->fPrimitiveProcessor.get(), state->getPipeline(), &state->fDesc,
                  &state->fBatchTracker);
    gpu->drawPath(args, this->path(), fStencilSettings);
}

void GrTargetCommands::DrawPaths::execute(GrGpu* gpu, const SetState* state) {
    SkASSERT(state);
    DrawArgs args(state->fPrimitiveProcessor.get(), state->getPipeline(), &state->fDesc,
                  &state->fBatchTracker);
    gpu->drawPaths(args, this->pathRange(),
                   fIndices, fIndexType,
                   fTransforms, fTransformType,
                   fCount, fStencilSettings);
}

void GrTargetCommands::DrawBatch::execute(GrGpu*, const SetState* state) {
    SkASSERT(state);
    fBatch->generateGeometry(fBatchTarget, state->getPipeline());
}

void GrTargetCommands::SetState::execute(GrGpu* gpu, const SetState*) {
    // TODO sometimes we have a prim proc, othertimes we have a GrBatch.  Eventually we
    // will only have GrBatch and we can delete this
    if (fPrimitiveProcessor) {
        gpu->buildProgramDesc(&fDesc, *fPrimitiveProcessor, *getPipeline(), fBatchTracker);
    }
}

void GrTargetCommands::Clear::execute(GrGpu* gpu, const SetState*) {
    if (GrColor_ILLEGAL == fColor) {
        gpu->discard(this->renderTarget());
    } else {
        gpu->clear(&fRect, fColor, fCanIgnoreRect, this->renderTarget());
    }
}

void GrTargetCommands::ClearStencilClip::execute(GrGpu* gpu, const SetState*) {
    gpu->clearStencilClip(fRect, fInsideClip, this->renderTarget());
}

void GrTargetCommands::CopySurface::execute(GrGpu* gpu, const SetState*) {
    gpu->copySurface(this->dst(), this->src(), fSrcRect, fDstPoint);
}

GrTargetCommands::Cmd* GrTargetCommands::recordCopySurface(GrSurface* dst,
                                                           GrSurface* src,
                                                           const SkIRect& srcRect,
                                                           const SkIPoint& dstPoint) {
    CopySurface* cs = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, CopySurface, (dst, src));
    cs->fSrcRect = srcRect;
    cs->fDstPoint = dstPoint;
    return cs;
}

bool GrTargetCommands::setupPipelineAndShouldDraw(GrInOrderDrawBuffer* iodb,
                                                  const GrPrimitiveProcessor* primProc,
                                                  const GrDrawTarget::PipelineInfo& pipelineInfo) {
    SetState* ss = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, SetState, (primProc));
    iodb->setupPipeline(pipelineInfo, ss->pipelineLocation()); 

    if (ss->getPipeline()->mustSkip()) {
        fCmdBuffer.pop_back();
        return false;
    }

    ss->fPrimitiveProcessor->initBatchTracker(&ss->fBatchTracker,
                                              ss->getPipeline()->getInitBatchTracker());

    if (fPrevState && fPrevState->fPrimitiveProcessor.get() &&
        fPrevState->fPrimitiveProcessor->canMakeEqual(fPrevState->fBatchTracker,
                                                      *ss->fPrimitiveProcessor,
                                                      ss->fBatchTracker) &&
        fPrevState->getPipeline()->isEqual(*ss->getPipeline())) {
        fCmdBuffer.pop_back();
    } else {
        fPrevState = ss;
        iodb->recordTraceMarkersIfNecessary(ss);
    }
    return true;
}

bool GrTargetCommands::setupPipelineAndShouldDraw(GrInOrderDrawBuffer* iodb,
                                                  GrBatch* batch,
                                                  const GrDrawTarget::PipelineInfo& pipelineInfo) {
    SetState* ss = GrNEW_APPEND_TO_RECORDER(fCmdBuffer, SetState, ());
    iodb->setupPipeline(pipelineInfo, ss->pipelineLocation()); 

    if (ss->getPipeline()->mustSkip()) {
        fCmdBuffer.pop_back();
        return false;
    }

    batch->initBatchTracker(ss->getPipeline()->getInitBatchTracker());

    if (fPrevState && !fPrevState->fPrimitiveProcessor.get() &&
        fPrevState->getPipeline()->isEqual(*ss->getPipeline())) {
        fCmdBuffer.pop_back();
    } else {
        fPrevState = ss;
        iodb->recordTraceMarkersIfNecessary(ss);
    }
    return true;
}

