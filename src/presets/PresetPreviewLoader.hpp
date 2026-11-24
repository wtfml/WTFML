#pragma once

#include "PresetApply.hpp"
#include "PresetStore.hpp"
#include "util/LatestReadLoader.hpp"

namespace Presets {
struct PreviewRequest {
    QString root;
    QString presetId;
    QString instanceId;
    QString instanceRoot;
    bool supportsDataPacks = true;
    bool gameRunning = false;
    bool operator==(const PreviewRequest &) const = default;
};

struct PreviewSnapshot {
    PreviewRequest request;
    PresetManifest manifest;
    ApplyPlan plan;
    QString problem;
    bool ok = false;
};

inline PreviewSnapshot preparePreview(const PreviewRequest &request) {
    PreviewSnapshot result;
    result.request = request;
    bool ok = false;
    QString reason;
    result.manifest = loadManifest(request.root, request.presetId, &ok, &reason);
    if (!ok) {
        result.problem = QStringLiteral("这一份读不出来：%1").arg(reason);
        return result;
    }
    auto target = readTargetSnapshot(request.instanceRoot, &result.problem);
    if (!result.problem.isEmpty()) return result;
    target.supportsDataPacks = request.supportsDataPacks;
    PlanRequest plan;
    plan.presetFilesRoot = filesDir(request.root, request.presetId);
    plan.sections = result.manifest.sections;
    plan.target = std::move(target);
    plan.gameRunning = request.gameRunning;
    result.plan = planPresetApplication(plan);
    result.ok = true;
    return result;
}

class PreviewLoader final : public LatestReadLoader<PreviewRequest, PreviewSnapshot> {
    using Base = LatestReadLoader<PreviewRequest, PreviewSnapshot>;
public:
    explicit PreviewLoader(QObject *parent, Reader reader = {})
        : Base(parent, [reader = reader ? std::move(reader) : Reader(preparePreview)](const PreviewRequest &request) {
            auto result = reader(request);
            result.request = request;
            return result;
        }) {}
};
} // namespace Presets
