#include "core/arena_match.h"

#include <algorithm>
#include <cmath>

#include "core/game_config.h"
#include "core/world.h"

namespace blob {

namespace {

uint16_t secondsCeil(double seconds) {
    if (seconds <= 0.0) {
        return 0;
    }
    return static_cast<uint16_t>(std::ceil(seconds));
}

}  // namespace

ArenaMatch::ArenaMatch(uint32_t seed, int countdownSec, int durationSec,
                       int shrinkStartSec)
    : roundSeed_(seed == 0 ? 1u : seed),
      countdownSec_(countdownSec > 0 ? countdownSec : 1),
      durationSec_(durationSec > 0 ? durationSec : 1),
      shrinkStartSec_(shrinkStartSec >= 0 ? shrinkStartSec : 0) {}

MatchInfo ArenaMatch::info(const World& world, std::size_t playerCount) const {
    MatchInfo out;
    out.phase = phase_;
    out.playerCount = static_cast<uint16_t>(
        std::min<std::size_t>(playerCount, 0xFFFFu));
    out.shrinkRadius = world.shrinkEnabled() ? world.shrinkRadius() : 0.0f;

    double remain = 0.0;
    switch (phase_) {
        case MatchPhase::Waiting:
            remain = 0.0;
            break;
        case MatchPhase::Countdown:
            remain = countdownSec_ - phaseElapsedSec_;
            break;
        case MatchPhase::Playing:
            remain = durationSec_ - totalElapsedSec_;
            break;
        case MatchPhase::Ended:
            remain = 0.0;
            break;
    }
    out.remainSec = secondsCeil(remain);
    return out;
}

void ArenaMatch::tick(World& world, float dt, std::size_t playerCount) {
    if (phase_ == MatchPhase::Ended) {
        return;
    }

    if (playerCount == 0) {
        if (phase_ != MatchPhase::Waiting) {
            phase_ = MatchPhase::Waiting;
            phaseElapsedSec_ = 0.0;
            totalElapsedSec_ = 0.0;
            shrinkStarted_ = false;
            world.setShrinkEnabled(false);
        }
    } else if (phase_ == MatchPhase::Waiting) {
        transitionTo(MatchPhase::Countdown, world);
    }

    if (phase_ == MatchPhase::Countdown) {
        phaseElapsedSec_ += static_cast<double>(dt);
        if (playerCount >= static_cast<std::size_t>(cfg::kArenaPlayers) ||
            phaseElapsedSec_ >= static_cast<double>(countdownSec_)) {
            transitionTo(MatchPhase::Playing, world);
        }
    } else if (phase_ == MatchPhase::Playing) {
        phaseElapsedSec_ += static_cast<double>(dt);
        totalElapsedSec_ += static_cast<double>(dt);

        if (!shrinkStarted_ &&
            totalElapsedSec_ >= static_cast<double>(shrinkStartSec_)) {
            shrinkStarted_ = true;
            world.setShrinkDurationMs(cfg::kArenaShrinkDurationMs);
            world.setShrinkEnabled(true);
        }

        if (totalElapsedSec_ >= static_cast<double>(durationSec_)) {
            finish(world);
            return;
        }
    }

    const MatchInfo current = info(world, playerCount);
    if (!sameInfo(lastInfo_, current)) {
        lastInfo_ = current;
        stateChanged_ = true;
    }
}

void ArenaMatch::transitionTo(MatchPhase next, World& world) {
    if (phase_ == next) {
        return;
    }
    phase_ = next;
    phaseElapsedSec_ = 0.0;

    if (next == MatchPhase::Playing) {
        totalElapsedSec_ = 0.0;
        shrinkStarted_ = false;
        world.setShrinkEnabled(false);
    }
}

void ArenaMatch::finish(World& world) {
    phase_ = MatchPhase::Ended;
    phaseElapsedSec_ = 0.0;
    world.setShrinkEnabled(false);
    result_ = buildStandings(world);
    resultPending_ = true;
    stateChanged_ = true;
}

bool ArenaMatch::consumeStateChanged(const World& world, std::size_t playerCount,
                                     MatchInfo& out) {
    const MatchInfo current = info(world, playerCount);
    if (!stateChanged_ && sameInfo(lastInfo_, current)) {
        return false;
    }
    out = current;
    lastInfo_ = current;
    stateChanged_ = false;
    return true;
}

bool ArenaMatch::consumeResult(std::vector<Standing>& out) {
    if (!resultPending_) {
        return false;
    }
    out.insert(out.end(), result_.begin(), result_.end());
    result_.clear();
    resultPending_ = false;
    return true;
}

std::vector<Standing> ArenaMatch::buildStandings(const World& world) {
    std::vector<Standing> out;
    for (const Entity& entity : world.entities()) {
        if (!entity.alive || entity.kind != EntityKind::Player) {
            continue;
        }
        Standing standing;
        standing.id = entity.id;
        standing.name = entity.name;
        standing.score = entity.score;
        standing.kills = entity.kills;
        standing.deaths = entity.deaths;
        standing.radius = entity.radius;
        out.push_back(std::move(standing));
    }

    std::sort(out.begin(), out.end(), [](const Standing& a, const Standing& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.kills != b.kills) {
            return a.kills > b.kills;
        }
        if (a.radius != b.radius) {
            return a.radius > b.radius;
        }
        return a.id < b.id;
    });
    return out;
}

bool ArenaMatch::sameInfo(const MatchInfo& a, const MatchInfo& b) {
    return a.phase == b.phase &&
           a.remainSec == b.remainSec &&
           a.playerCount == b.playerCount &&
           std::fabs(a.shrinkRadius - b.shrinkRadius) < 0.5f;
}

}  // namespace blob
