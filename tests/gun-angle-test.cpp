// Compiles production settings/INI/menu and pose statements, with only file I/O replaced.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define WARN_LOG(...) ((void)0)
#define stricmp _stricmp
namespace hostfs { class File; }
#include "under_test.inc"

int failures = 0, checks = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
bool near(glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < 0.00002f; }

std::string fixture(const std::string& angle, const std::string& marker = "") {
    return "[config]\nvr.GunAngle = " + angle + "\n" + marker +
        "vr.Reload = 1\nSentinel = 73\nROMPath = E:/roms/untouched.zip\n"
        "[game-a]\nconfig.vr.GunAngle = 24\nconfig.Sentinel = 19\n"
        "[game-b]\nconfig.vr.GunAngle = -27\nconfig.Sentinel = 11\n";
}
void restart(const std::string& data) {
    config::Settings::instance().reset();
    config::cfgdb = config::IniFile();
    config::cfgdb.load(data);
    config::persisted = data;
    config::writeCount = 0;
    config::Settings::instance().load(false);
}
void settingsTests() {
    for (const std::string old : {"-10", "0", "17", "-2147483648", "2147483647", "invalid", "999999999999999999999"}) {
        restart(fixture(old));
        check((int)config::VrGunAngle == 0, "legacy " + old + " resets to displayed zero after Settings::load");
        check(config::loadInt("config", "vr.GunAngle", 99) == 0, "legacy " + old + " zero persists immediately");
        check(config::loadInt("config", "vr.GunAngleZeroVersion", 0) == 1, "legacy " + old + " migration marker persists");
        check(config::loadInt("config", "Sentinel", 0) == 73 && (int)config::VrReload == 1,
            "legacy " + old + " leaves other settings unchanged");
        check(config::loadStr("config", "ROMPath") == "E:/roms/untouched.zip", "ROM path preserved");
        check(config::loadInt("game-a", "config.vr.GunAngle", 0) == 24, "unrelated legacy section preserved");
        const std::string migrated = config::persisted;
        restart(migrated);
        check((int)config::VrGunAngle == 0 && config::writeCount == 0, "cold restart does not migrate twice");
        auto& settings = config::Settings::instance();
        settings.setGameId("game-a"); settings.load(true);
        check((int)config::VrGunAngle == 0 && (int)config::Sentinel == 19, "old per-game angle ignored; other game setting loads");
        chooseAngle(1); chooseAngle(1); chooseAngle(1);
        check((int)config::VrGunAngle == 3 && config::loadInt("config", "vr.GunAngle", 99) == 3,
            "later menu choices save in displayed degrees");
        settings.setGameId("game-b"); settings.load(true);
        check((int)config::VrGunAngle == 3 && (int)config::Sentinel == 11, "switch game preserves manual angle");
        const int writes = config::writeCount;
        settings.load(false);
        check((int)config::VrGunAngle == 3 && config::writeCount == writes, "reload is idempotent after manual choice");
        config::VrGunAngle.save();
        restart(config::persisted);
        check((int)config::VrGunAngle == 3, "later user angle survives real INI save/load");
    }
    restart(fixture("-21", "vr.GunAngleZeroVersion = 2\n"));
    check((int)config::VrGunAngle == -21 && config::loadInt("config", "vr.GunAngleZeroVersion") == 2,
        "future marker is neither migrated nor downgraded");
    restart(fixture("12", "vr.GunAngleZeroVersion = -1\n"));
    check((int)config::VrGunAngle == 0, "older marker migrates");
    for (int angle : {-30, 0, 30}) {
        restart(fixture(std::to_string(angle), "vr.GunAngleZeroVersion = 1\n"));
        check(gunAngle() == angle, "displayed menu value remains literal " + std::to_string(angle));
    }
    restart(fixture("30", "vr.GunAngleZeroVersion = 1\n"));
    chooseAngle(1);
    check((int)config::VrGunAngle == 30 && config::writeCount == 0, "menu upper bound clamps without save");
    chooseAngle(-1);
    check((int)config::VrGunAngle == 29, "menu remains one degree per step");
    config::VrGunAngle.set(-30); chooseAngle(-1);
    check((int)config::VrGunAngle == -30, "menu lower bound clamps");
}
void poseTests() {
    const glm::vec3 palm(0.025f, -0.05f, 0.1f), muzzle(0.1f, 0.02f, -0.4f);
    const glm::mat4 pose = glm::translate(glm::mat4(1.f), glm::vec3(0.3f, 1.2f, -0.5f));
    const glm::mat4 placement = glm::translate(glm::mat4(1.f), palm) * glm::scale(glm::mat4(1.f), glm::vec3(0.8f));
    struct Case { int displayed; glm::vec3 forward; };
    const Case cases[] {
        {0, {0, -0.173648178f, -0.984807753f}},
        {-30, {0, -0.642787610f, -0.766044443f}},
        {30, {0, 0.342020143f, -0.939692621f}},
        {std::numeric_limits<int>::min(), {0, -0.642787610f, -0.766044443f}},
        {std::numeric_limits<int>::max(), {0, 0.342020143f, -0.939692621f}}
    };
    for (const auto& c : cases) {
        const auto p = productionPose(c.displayed, palm, muzzle, pose, glm::quat(1.f, 0.f, 0.f, 0.f), placement);
        const std::string value = std::to_string(c.displayed);
        check(near(p.view.restForward, c.forward), "displayed " + value + " has baseline physical ray");
        check(near(glm::vec3(p.tilt * glm::vec4(palm, 1.f)), palm), "tilt preserves grip pivot " + value);
        check(near(glm::vec3(p.view.pose * glm::vec4(muzzle, 1.f)), p.view.restMuzzle), "drawn model and rest muzzle agree " + value);
        check(near(p.ray.origin, p.view.restMuzzle) && near(p.ray.dir, p.view.restForward), "UI pointer uses the gun ray " + value);
        check(near(p.view.aimPoint, p.view.restMuzzle + c.forward * 3.f), "laser endpoint uses the gun ray " + value);
    }
    const auto zero = productionPose(0, palm, muzzle, pose, glm::quat(1.f, 0.f, 0.f, 0.f), placement);
    const glm::mat4 previousMinusTen = glm::translate(glm::mat4(1.f), palm)
        * glm::rotate(glm::mat4(1.f), glm::radians(-10.f), glm::vec3(1, 0, 0))
        * glm::translate(glm::mat4(1.f), -palm);
    check(near(zero.view.restMuzzle, glm::vec3(pose * previousMinusTen * placement * glm::vec4(muzzle, 1.f))),
        "new zero exactly matches previous minus-ten physical muzzle");
    const glm::quat turn = glm::angleAxis(glm::radians(90.f), glm::vec3(0, 1, 0));
    const auto turned = productionPose(0, -palm, muzzle, glm::mat4_cast(turn), turn, placement);
    check(near(turned.view.restForward, {-0.984807753f, -0.173648178f, 0.f}), "baseline follows controller orientation");
}
int main() {
    settingsTests(); poseTests();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
