#include "IngameState.hpp"
#include "DebugState.hpp"
#include "DrawUI.hpp"
#include "PauseState.hpp"
#include "RWGame.hpp"

#include <ai/PlayerController.hpp>
#include <data/Model.hpp>
#include <data/WeaponData.hpp>
#include <dynamics/CollisionInstance.hpp>
#include <dynamics/RaycastCallbacks.hpp>
#include <engine/GameState.hpp>
#include <engine/GameWorld.hpp>
#include <objects/CharacterObject.hpp>
#include <objects/ItemPickup.hpp>
#include <objects/VehicleObject.hpp>
#include <script/ScriptMachine.hpp>

#include <glm/gtc/constants.hpp>

constexpr float kAutoLookTime = 2.f;
constexpr float kAutolookMinVelocity = 0.2f;
const float kInGameFOV = glm::half_pi<float>();
const float kMaxRotationRate = glm::half_pi<float>();
const float kCameraPitchLimit = glm::quarter_pi<float>() * 0.5f;
const float kVehicleCameraPitch =
    glm::half_pi<float>() - glm::quarter_pi<float>() * 0.25f;

IngameState::IngameState(RWGame* game, bool newgame, const std::string& save)
    : State(game)
    , started(false)
    , save(save)
    , newgame(newgame)
    , autolookTimer(0.f)
    , camMode(IngameState::CAMERA_NORMAL)
    , m_cameraAngles{0.f, glm::half_pi<float>()}
    , m_invertedY(game->getConfig().getInputInvertY())
    , m_vehicleFreeLook(true) {
    _look.frustum.fov = kInGameFOV;
}

void IngameState::startTest() {
    auto playerChar = getWorld()->createPlayer({270.f, -605.f, 40.f});

    getWorld()->state->playerObject = playerChar->getGameObjectID();

    glm::vec3 itemspawn(276.5f, -609.f, 36.5f);
    for (unsigned int i = 1; i < getWorld()->data->weaponData.size(); ++i) {
        auto& item = getWorld()->data->weaponData[i];
        getWorld()->createPickup(itemspawn, item->modelID,
                                 PickupObject::OnStreet);
        itemspawn.x += 2.5f;
    }

    auto carPos = glm::vec3(286.f, -591.f, 37.f);
    auto carRot = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 0.f, 1.f));
    // Landstalker, Stinger, Linerunner, Trash, Bobcat
    const std::vector<int> kTestVehicles = {90, 92, 93, 98, 111};
    for (auto id : kTestVehicles) {
        getWorld()->createVehicle(id, carPos, carRot);
        carPos += carRot * glm::vec3(5.f, 0.f, 0.f);
    }
}

void IngameState::startGame() {
    game->startScript("data/main.scm");
    game->getScriptVM()->startThread(0);
    getWorld()->sound.playBackground(getWorld()->data->getDataPath() +
                                     "/audio/City.wav");
}

void IngameState::enter() {
    if (!started) {
        if (newgame) {
            if (save.empty()) {
                startGame();
            } else if (save == "test") {
                startTest();
            } else {
                game->loadGame(save);
            }
        }
        started = true;
    }

    getWindow().hideCursor();
}

void IngameState::exit() {
}

void IngameState::tick(float dt) {
    auto world = getWorld();
    autolookTimer = std::max(autolookTimer - dt, 0.f);

    // Update displayed money value
    // @todo the game uses another algorithm which is non-linear
    {
        float moneyFrequency = 1.0f / 30.0f;
        moneyTimer += dt;
        while (moneyTimer >= moneyFrequency) {
            int32_t difference = world->state->playerInfo.money -
                                 world->state->playerInfo.displayedMoney;

            // Generates 0, 1 (difference < 100), 12 (difference < 1000), 123
            // (difference < 10000), .. etc.
            // Negative input will result in negative output
            auto GetIncrement = [](int32_t difference) -> int32_t {
                // @todo is this correct for difference >= 1000000000 ?
                int32_t r = 1;
                int32_t i = 2;
                if (difference == 0) {
                    return 0;
                }
                while (std::abs(difference) >= 100) {
                    difference /= 10;
                    r = r * 10 + i;
                    i++;
                }
                return (difference < 0) ? -r : r;
            };
            world->state->playerInfo.displayedMoney += GetIncrement(difference);

            moneyTimer -= moneyFrequency;
        }
    }

    auto player = game->getPlayer();

    if (player) {
        // Force all input to 0 if player input is disabled
        /// @todo verify 0ing input is the correct behaviour
        const auto inputEnabled = player->isInputEnabled();

        auto input = [&](GameInputState::Control c) {
            return inputEnabled ? world->state->input[0][c] : 0.f;
        };
        auto pressed = [&](GameInputState::Control c) {
            return inputEnabled && world->state->input[0].pressed(c) &&
                   !world->state->input[1].pressed(c);
        };
        auto held = [&](GameInputState::Control c) {
            return inputEnabled && world->state->input[0].pressed(c);
        };

        auto target = world->pedestrianPool.find(world->state->cameraTarget);

        if (target == nullptr) {
            target = player->getCharacter();
        }

        glm::vec3 targetPosition = target->getPosition();
        glm::vec3 lookTargetPosition = targetPosition;
        targetPosition += glm::vec3(0.f, 0.f, 1.f);
        lookTargetPosition += glm::vec3(0.f, 0.f, 0.5f);

        auto vehicle =
            (target->type() == GameObject::Character)
                ? static_cast<CharacterObject*>(target)->getCurrentVehicle()
                : nullptr;
        if (vehicle) {
            // Rotate the camera to the ideal angle if the player isn't moving
            // it
            float velocity = vehicle->getVelocity();
            if (autolookTimer <= 0.f &&
                glm::abs(velocity) > kAutolookMinVelocity) {
                auto idealYaw = glm::roll(vehicle->getRotation());
                const auto idealPitch = kVehicleCameraPitch;
                if (velocity < 0.f) {
                    idealYaw = glm::mod(idealYaw - glm::pi<float>(),
                                        glm::pi<float>() * 2.f);
                }
                float currentYaw =
                    glm::mod(m_cameraAngles.x, glm::pi<float>() * 2);
                float currentPitch = m_cameraAngles.y;
                float deltaYaw = idealYaw - currentYaw;
                float deltaPitch = idealPitch - currentPitch;
                if (glm::abs(deltaYaw) > glm::pi<float>()) {
                    deltaYaw -= glm::sign(deltaYaw) * glm::pi<float>() * 2.f;
                }
                m_cameraAngles.x +=
                    glm::sign(deltaYaw) *
                    std::min(kMaxRotationRate * dt, glm::abs(deltaYaw));
                m_cameraAngles.y +=
                    glm::sign(deltaPitch) *
                    std::min(kMaxRotationRate * dt, glm::abs(deltaPitch));
            }
        }

        glm::vec3 movement;
        movement.x = input(GameInputState::GoForward) -
                     input(GameInputState::GoBackwards);
        movement.y =
            input(GameInputState::GoLeft) - input(GameInputState::GoRight);
        /// @todo replace with correct sprint behaviour
        float speed = held(GameInputState::Sprint) ? 2.f : 1.f;

        player->setRunning(!held(GameInputState::Walk));
        /// @todo find the correct behaviour for entering & exiting
        if (pressed(GameInputState::EnterExitVehicle)) {
            /// @todo move me
            if (player->getCharacter()->getCurrentVehicle()) {
                player->exitVehicle();
            } else if (!player->isCurrentActivity(
                           Activities::EnterVehicle::ActivityName)) {
                player->enterNearestVehicle();
            }
        } else if (glm::length2(movement) > 0.001f) {
            if (player->isCurrentActivity(
                    Activities::EnterVehicle::ActivityName)) {
                // Give up entering a vehicle if we're alreadying doing so
                player->skipActivity();
            }
        }

        if (player->getCharacter()->getCurrentVehicle()) {
            auto vehicle = player->getCharacter()->getCurrentVehicle();
            vehicle->setHandbraking(held(GameInputState::Handbrake));
            player->setMoveDirection(movement);
        } else {
            if (pressed(GameInputState::Jump)) {
                player->jump();
            }

            float length = glm::length(movement);
            if (length > 0.1f) {
                auto move = speed * glm::normalize(movement);
                player->setMoveDirection(glm::vec3(move.x, 0.f, move.y));
            } else {
                player->setMoveDirection(glm::vec3(0.f));
            }
            player->setLookDirection(m_cameraAngles);
        }
    }
}

void IngameState::draw(GameRenderer* r) {
    if (!getWorld()->state->isCinematic && getWorld()->isCutsceneDone()) {
        drawHUD(_look, game->getPlayer(), getWorld(), r);
    }

    State::draw(r);
}

void IngameState::handleEvent(const SDL_Event& event) {
    auto player = game->getPlayer();

    switch (event.type) {
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                    StateManager::get().enter<PauseState>(game);
                    break;
                case SDLK_m:
                    StateManager::get().enter<DebugState>(game, _look.position,
                                                          _look.rotation);
                    break;
                case SDLK_SPACE:
                    if (getWorld()->state->currentCutscene) {
                        getWorld()->state->skipCutscene = true;
                    }
                    break;
                case SDLK_c:
                    camMode =
                        CameraMode((camMode + (CameraMode)1) % CAMERA_MAX);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    if (player && player->isInputEnabled()) {
        handlePlayerInput(event);
    }
    State::handleEvent(event);
}

void IngameState::handlePlayerInput(const SDL_Event& event) {
    auto player = game->getPlayer();
    switch (event.type) {
        case SDL_MOUSEBUTTONDOWN:
            switch (event.button.button) {
                case SDL_BUTTON_LEFT:
                    player->getCharacter()->useItem(true, true);
                    break;
                default:
                    break;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            switch (event.button.button) {
                case SDL_BUTTON_LEFT:
                    player->getCharacter()->useItem(false, true);
                    break;
                default:
                    break;
            }
            break;
        case SDL_MOUSEWHEEL:
            player->getCharacter()->cycleInventory(event.wheel.y > 0);
            break;
        case SDL_MOUSEMOTION:
            if (game->hasFocus()) {
                glm::ivec2 screenSize = getWindow().getSize();
                glm::vec2 mouseMove(
                    event.motion.xrel / static_cast<float>(screenSize.x),
                    event.motion.yrel / static_cast<float>(screenSize.y));

                autolookTimer = kAutoLookTime;
                if (!m_invertedY) {
                    mouseMove.y = -mouseMove.y;
                }
                m_cameraAngles += glm::vec2(-mouseMove.x, mouseMove.y);
                m_cameraAngles.y =
                    glm::clamp(m_cameraAngles.y, kCameraPitchLimit,
                               glm::pi<float>() - kCameraPitchLimit);
            }
            break;
        default:
            break;
    }
}

bool IngameState::shouldWorldUpdate() {
    return true;
}

const ViewCamera& IngameState::getCamera(float alpha) {
    auto player = game->getPlayer();
    auto world = getWorld();

    if (!player) {
        return _look;
    }

    // Force all input to 0 if player input is disabled
    /// @todo verify 0ing input is the correct behaviour
    const auto inputEnabled = player->isInputEnabled();

    auto held = [&](GameInputState::Control c) {
        return inputEnabled && world->state->input[0].pressed(c);
    };

    float viewDistance = getViewDistance();
    auto target = getCameraTarget();
    bool lookleft = held(GameInputState::LookLeft);
    bool lookright = held(GameInputState::LookRight);
    btCollisionObject* physTarget = player->getCharacter()->physObject;

    auto targetTransform = target->getTimeAdjustedTransform(alpha);

    glm::vec3 targetPosition(targetTransform[3]);
    glm::vec3 lookTargetPosition(targetPosition);
    targetPosition += glm::vec3(0.f, 0.f, 1.f);
    lookTargetPosition += glm::vec3(0.f, 0.f, 0.5f);

    if (target->type() == GameObject::Vehicle) {
        auto vehicle = (VehicleObject*)target;
        auto model = vehicle->getModel();
        auto maxDist = model->getBoundingRadius() * 2.f;
        viewDistance = viewDistance + maxDist;
        lookTargetPosition.z += (vehicle->info->handling.dimensions.z * 0.5f);
        targetPosition.z += (vehicle->info->handling.dimensions.z * 0.5f);
        physTarget = vehicle->collision->getBulletBody();

        if (!m_vehicleFreeLook) {
            m_cameraAngles.y = kVehicleCameraPitch;
        }
    }

    // Handle top-down camera
    if (camMode == CAMERA_TOPDOWN) {
        cameraPosition = targetPosition + glm::vec3(0.f, 0.f, viewDistance);
        _look.rotation =
            glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.f, 1.f, 0.f));
    } else if ((lookleft || lookright) &&
               target->type() == GameObject::Vehicle) {
        auto rotation = target->getRotation();
        if (!lookright) {
            rotation *= glm::angleAxis(glm::half_pi<float>(),
                                       glm::vec3(0.f, 0.f, -1.f));
        } else if (!lookleft) {
            rotation *=
                glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.f, 0.f, 1.f));
        }
        cameraPosition =
            targetPosition + rotation * glm::vec3(0.f, viewDistance, 0.f);
    } else {
        // Determine the "ideal" camera position for the current view angles
        auto yaw = glm::angleAxis(m_cameraAngles.x - glm::half_pi<float>(),
                                  glm::vec3(0.f, 0.f, 1.f));
        auto pitch = glm::angleAxis(m_cameraAngles.y, glm::vec3(0.f, 1.f, 0.f));
        auto cameraOffset = yaw * pitch * glm::vec3(0.f, 0.f, viewDistance);
        cameraPosition = targetPosition + cameraOffset;
    }

    auto lookdir = glm::normalize(lookTargetPosition - cameraPosition);
    // Calculate the angles to look at the target position
    float len2d = glm::length(glm::vec2(lookdir));
    float anglePitch = glm::atan(lookdir.z, len2d);
    float angleYaw = glm::atan(lookdir.y, lookdir.x);
    glm::quat angle(glm::vec3(0.f, -anglePitch, angleYaw));

    // Ensure the target position is actually visible
    auto rayEnd = cameraPosition;
    auto rayStart = targetPosition;
    auto to = btVector3(rayEnd.x, rayEnd.y, rayEnd.z);
    auto from = btVector3(rayStart.x, rayStart.y, rayStart.z);
    ClosestNotMeRayResultCallback ray(physTarget, from, to);

    world->dynamicsWorld->rayTest(from, to, ray);
    if (ray.hasHit() && ray.m_closestHitFraction < 1.f) {
        cameraPosition =
            glm::vec3(ray.m_hitPointWorld.x(), ray.m_hitPointWorld.y(),
                      ray.m_hitPointWorld.z());
        cameraPosition +=
            glm::vec3(ray.m_hitNormalWorld.x(), ray.m_hitNormalWorld.y(),
                      ray.m_hitNormalWorld.z()) *
            0.1f;
    }
    _look.position = cameraPosition;
    _look.rotation = angle;
    return _look;
}

GameObject* IngameState::getCameraTarget() const {
    auto target =
        getWorld()->pedestrianPool.find(game->getState()->cameraTarget);

    if (target == nullptr && game->getPlayer()) {
        target = game->getPlayer()->getCharacter();
    }

    // If the target is a character in a vehicle, make the vehicle the target
    if (target && target->type() == GameObject::Character) {
        auto vehicle = ((CharacterObject*)target)->getCurrentVehicle();
        if (vehicle) {
            target = vehicle;
        }
    }

    return target;
}

float IngameState::getViewDistance() const {
    float viewDistance = 4.f;
    switch (camMode) {
        case IngameState::CAMERA_CLOSE:
            viewDistance = 2.f;
            break;
        case IngameState::CAMERA_NORMAL:
            viewDistance = 4.0f;
            break;
        case IngameState::CAMERA_FAR:
            viewDistance = 6.f;
            break;
        case IngameState::CAMERA_TOPDOWN:
            viewDistance = 15.f;
            break;
        default:
            viewDistance = 4.f;
    }
    return viewDistance;
}
