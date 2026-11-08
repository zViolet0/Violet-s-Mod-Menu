#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCParticleSystem.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>

#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <random>
#include <chrono>
#include <deque>
#include <sstream>
#include <cstdio>


using namespace geode::prelude;

// ---- Speedhack range ----
constexpr float SPEED_MIN = 0.1f;
constexpr float SPEED_MAX = 3.0f;

// ---- Slider layout (offsets from the bottom of the popup) ----
constexpr float SLIDER_Y = 45.f;
constexpr float LABEL_Y  = 20.f;
constexpr float POPUP_HEIGHT = 280.f;  // fixed size, so pages don't resize it
constexpr int   SPEED_PAGE = 0;        // the page the speed slider shows on

// ---- Pages ----
static const std::array<const char*, 5> PAGE_NAMES = {
    "Player", "Visuals", "Misc", "Labels", "Bot"
};

// ---- Hack list: add a new line here to get a new toggle ----
struct HackEntry {
    const char* id;
    const char* label;
    int page;
};

// ---- Hacks that's in the menu (will be adding more in the future) ----
static const std::array<HackEntry, 21> HACKS = {{
    {"noclip",             "Noclip",             0},
    {"speedhack",          "Speedhack",          0},
    {"startpos-switcher",  "StartPos Switcher",  0},
    {"frame-advance",      "Frame Advance",      0},
    {"show-hitboxes",      "Show Hitboxes",      1},
    {"no-wave-pulse",      "No Wave Pulse",      1},
    {"no-trail",           "No Trail",           1},
    {"no-particles",       "No Particles",       1},
    {"no-respawn-flash",   "No Respawn Flash",   1},
    {"icons",              "Icons",              2},
    {"instant-complete",   "Instant Complete",   2},
    {"verify-hack",        "Verify Hack",        2},
    {"copy-hack",          "Copy Hack",          2},
    {"jumpscare",          "Jumpscare",          2},
    {"fps-label",          "FPS Counter",        3},
    {"cps-label",          "CPS Counter",        3},
    {"cheat-indicator",    "Cheat Indicator",    3},
    {"auto-safe-mode",     "Auto Safe-Mode",     3},
    {"safe-mode",          "Safe-Mode",          3},
    {"bot-record",         "Record",             4},
    {"bot-replay",         "Replay",             4},
}};
//  ^^^^                    ^^^^
// actual name             name that shows up
// used for the            in the menu :))
// code

// Use this from any hook: if (hackOn("noclip")) { ... }
static bool hackOn(std::string const& id) {
    return Mod::get()->getSavedValue<bool>(id, false);
}

constexpr int   JUMPSCARE_ONE_IN = 200;   // 1 in this many deaths
constexpr float JUMPSCARE_HOLD   = 1.2f;  // seconds fully visible
constexpr float JUMPSCARE_FADE   = 1.0f;  // seconds to fade out

static bool rollJumpscare() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_int_distribution<int>(1, JUMPSCARE_ONE_IN)(rng) == 1;
}

// Click timestamps, filled by the pushButton hook below
static std::deque<std::chrono::steady_clock::time_point> g_clicks;
static std::chrono::steady_clock::time_point g_lastClick = std::chrono::steady_clock::time_point{};

static float getSpeed() {
    return Mod::get()->getSavedValue<float>("speedhack-value", 1.0f);
}

static bool g_safeCompletion = false;
static bool g_badHackUsed = false;

// Hacks that make a run unsafe. Edit this list freely.
static bool badHackActive() {
    if (hackOn("noclip") || hackOn("show-hitboxes") || hackOn("frame-advance") ||
        hackOn("speedhack") || hackOn("instant-complete") || hackOn("bot-replay")) return true;

    // StartPos Switcher only counts while you're on a startpos (not "0")
    if (hackOn("startpos-switcher")) {
        auto pl = PlayLayer::get();
        if (pl && pl->m_startPosObject) return true;
    }
    return false;
}

// Sets the music playback speed to match the game speed
static void syncMusicPitch(float speed) {
    auto engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;

    if (engine->m_backgroundMusicChannel) {
        engine->m_backgroundMusicChannel->setPitch(speed);
    }
}

// ---- Speedhack: on = slider speed, off = normal 1x (read every frame) ----
class $modify(HackScheduler, CCScheduler) {
    void update(float dt) {
        bool on = hackOn("speedhack");
        float speed = on ? getSpeed() : 1.0f;

        // Keep music in sync while on, plus one final reset to 1x when it's turned off.
        // Setting it every frame also covers new music starting (restarts, new levels),
        // which would otherwise come back at normal pitch.
        static bool wasOn = false;
        if (on || wasOn) syncMusicPitch(speed);
        wasOn = on;

        CCScheduler::update(dt * speed);
    }
};

constexpr int BOT_PAGE = 4;  // index of "Bot" in PAGE_NAMES

static void saveReplayFile();
static void loadReplayFile();

// ---- The popup ----
class HackMenu : public geode::Popup {
protected:
    int m_page = 0;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCLabelBMFont* m_speedLabel = nullptr;
    std::vector<CCNode*> m_pageNodes;  // everything on the current page, so we can clear it

    bool init() {
        if (!Popup::init(300.f, POPUP_HEIGHT)) return false;

        this->setTitle("Violet's Mod Menu");

        // Page name + number, under the title
        m_pageLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_pageLabel->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_pageLabel, Anchor::Top, ccp(0.f, -38.f));

        // Arrows on the left and right edges
        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto leftBtn = CCMenuItemSpriteExtra::create(
            leftSpr, this, menu_selector(HackMenu::onPrev)
        );
        leftBtn->setPosition(ccp(-15.f, m_size.height / 2.f));
        m_buttonMenu->addChild(leftBtn);

        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setFlipX(true);
        auto rightBtn = CCMenuItemSpriteExtra::create(
            rightSpr, this, menu_selector(HackMenu::onNext)
        );
        rightBtn->setPosition(ccp(m_size.width + 15.f, m_size.height / 2.f));
        m_buttonMenu->addChild(rightBtn);

        this->buildPage();

        s_open = this;
        return true;
    }

    // Clears the current page and builds the one in m_page
    void buildPage() {
        for (auto node : m_pageNodes) node->removeFromParent();
        m_pageNodes.clear();
        m_speedLabel = nullptr;

        m_pageLabel->setString(fmt::format(
            "{} ({}/{})", PAGE_NAMES[m_page], m_page + 1, PAGE_NAMES.size()
        ).c_str());

        float top = m_size.height - 65.f;
        int row = 0;
        for (size_t i = 0; i < HACKS.size(); i++) {
            if (HACKS[i].page != m_page) continue;
            float y = top - row * 40.f;
            row++;

            auto toggler = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(HackMenu::onToggle), 0.7f
            );
            toggler->setTag(static_cast<int>(i));  // index into HACKS
            toggler->toggle(hackOn(HACKS[i].id));
            toggler->setPosition(ccp(40.f, y));
            m_buttonMenu->addChild(toggler);
            m_pageNodes.push_back(toggler);

            auto label = CCLabelBMFont::create(HACKS[i].label, "bigFont.fnt");
            label->setScale(0.6f);
            label->setAnchorPoint(ccp(0.f, 0.5f));
            label->setPosition(ccp(70.f, y));
            m_buttonMenu->addChild(label);
            m_pageNodes.push_back(label);
        }

        // The Bot page also gets Save / Load buttons under its toggles
        if (m_page == BOT_PAGE) {
            float y = top - row * 40.f - 10.f;

            auto saveBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Save"), this, menu_selector(HackMenu::onSaveReplay)
            );
            saveBtn->setPosition(ccp(90.f, y));
            m_buttonMenu->addChild(saveBtn);
            m_pageNodes.push_back(saveBtn);

            auto loadBtn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Load"), this, menu_selector(HackMenu::onLoadReplay)
            );
            loadBtn->setPosition(ccp(210.f, y));
            m_buttonMenu->addChild(loadBtn);
            m_pageNodes.push_back(loadBtn);
        }

        // The speed slider only shows on its own page
        if (m_page == SPEED_PAGE) {
            auto slider = Slider::create(this, menu_selector(HackMenu::onSpeedSlider), 0.8f);
            slider->setValue((getSpeed() - SPEED_MIN) / (SPEED_MAX - SPEED_MIN));
            m_mainLayer->addChildAtPosition(slider, Anchor::Bottom, ccp(0.f, SLIDER_Y));
            m_pageNodes.push_back(slider);

            m_speedLabel = CCLabelBMFont::create(
                fmt::format("{:.2f}x", getSpeed()).c_str(), "bigFont.fnt"
            );
            m_speedLabel->setScale(0.5f);
            m_mainLayer->addChildAtPosition(m_speedLabel, Anchor::Bottom, ccp(0.f, LABEL_Y));
            m_pageNodes.push_back(m_speedLabel);
        }
    }

    void onPrev(CCObject*) {
        int count = static_cast<int>(PAGE_NAMES.size());
        m_page = (m_page + count - 1) % count;
        this->buildPage();
    }

    void onNext(CCObject*) {
        int count = static_cast<int>(PAGE_NAMES.size());
        m_page = (m_page + 1) % count;
        this->buildPage();
    }

    void onToggle(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        auto const& hack = HACKS[toggler->getTag()];
        // The callback fires before the visual state flips, hence the "!"
        Mod::get()->setSavedValue<bool>(hack.id, !toggler->isToggled());
    }

    void onSaveReplay(CCObject*) { saveReplayFile(); }
    void onLoadReplay(CCObject*) { loadReplayFile(); }

    void onSpeedSlider(CCObject* sender) {
        float value = static_cast<SliderThumb*>(sender)->getValue();  // 0 to 1
        float speed = SPEED_MIN + value * (SPEED_MAX - SPEED_MIN);

        Mod::get()->setSavedValue<float>("speedhack-value", speed);
        if (m_speedLabel) {
            m_speedLabel->setString(fmt::format("{:.2f}x", speed).c_str());
        }
    }

public:
    // Tracks the currently open menu so TAB can close it again
    static inline HackMenu* s_open = nullptr;

    ~HackMenu() {
        if (s_open == this) s_open = nullptr;
    }

    static HackMenu* create() {
        auto ret = new HackMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Opens the menu, or closes it if it's already open
static void toggleHackMenu() {
    if (HackMenu::s_open) {
        HackMenu::s_open->onClose(nullptr);
        return;
    }
    if (auto menu = HackMenu::create()) menu->show();
}

// ---- Hitbox drawing helper ----
static void drawBox(CCDrawNode* node, CCRect const& r, ccColor4F border) {
    CCPoint verts[4] = {
        ccp(r.getMinX(), r.getMinY()),
        ccp(r.getMinX(), r.getMaxY()),
        ccp(r.getMaxX(), r.getMaxY()),
        ccp(r.getMaxX(), r.getMinY()),
    };
    // Transparent fill, colored outline
    node->drawPolygon(verts, 4, ccColor4F{0.f, 0.f, 0.f, 0.f}, 0.5f, border);
}

static void drawCircle(CCDrawNode* node, CCPoint center, float radius, ccColor4F border) {
    constexpr int SEGMENTS = 24;
    CCPoint verts[SEGMENTS];
    for (int i = 0; i < SEGMENTS; i++) {
        float angle = (static_cast<float>(i) / SEGMENTS) * 2.f * static_cast<float>(M_PI);
        verts[i] = ccp(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
    }
    node->drawPolygon(verts, SEGMENTS, ccColor4F{0.f, 0.f, 0.f, 0.f}, 0.5f, border);
}

static void drawQuad(CCDrawNode* node, CCPoint a, CCPoint b, CCPoint c, CCPoint d, ccColor4F border) {
    CCPoint verts[4] = {a, b, c, d};
    node->drawPolygon(verts, 4, ccColor4F{0.f, 0.f, 0.f, 0.f}, 0.5f, border);
}

static void drawSlope(CCDrawNode* node, GameObject* obj, ccColor4F border) {
    auto rect = obj->getObjectRect();
    CCPoint center = ccp(rect.getMidX(), rect.getMidY());

    float rotation = obj->getRotation();
    float w = rect.size.width;
    float h = rect.size.height;

    // At 90/270 degrees the rect is already swapped, so swap back to get the unrotated size
    if (std::fmod(std::abs(rotation), 180.f) > 45.f) std::swap(w, h);

    // Unrotated slope: right angle at the bottom-right corner
    CCPoint local[3] = {
        ccp(-w / 2.f, -h / 2.f),
        ccp( w / 2.f, -h / 2.f),
        ccp( w / 2.f,  h / 2.f),
    };

    float rad = rotation * static_cast<float>(M_PI) / 180.f;
    float c = std::cos(rad), s = std::sin(rad);

    CCPoint verts[3];
    for (int i = 0; i < 3; i++) {
        float x = local[i].x * (obj->isFlipX() ? -1.f : 1.f);
        float y = local[i].y * (obj->isFlipY() ? -1.f : 1.f);
        // cocos rotation is clockwise
        verts[i] = ccp(center.x + x * c + y * s, center.y - x * s + y * c);
    }
    node->drawPolygon(verts, 3, ccColor4F{0.f, 0.f, 0.f, 0.f}, 0.5f, border);
}

// ---- StartPos Switcher ----
static std::vector<StartPosObject*> getStartPositions(PlayLayer* pl) {
    std::vector<StartPosObject*> list;
    if (!pl || !pl->m_objects) return list;

    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (auto sp = typeinfo_cast<StartPosObject*>(obj)) list.push_back(sp);
    }
    // Left to right
    std::sort(list.begin(), list.end(), [](StartPosObject* a, StartPosObject* b) {
        return a->getPositionX() < b->getPositionX();
    });
    return list;
}

// 0 = no startpos (normal level start), 1..n = the startpos objects
static int currentStartPosIndex(PlayLayer* pl, std::vector<StartPosObject*> const& list) {
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i] == pl->m_startPosObject) return static_cast<int>(i) + 1;
    }
    return 0;
}

// Updates the "3/10" text and shows or hides the whole UI
static void refreshStartPosUI(PlayLayer* pl) {
    if (!pl || !pl->m_uiLayer) return;

    auto label = static_cast<CCLabelBMFont*>(pl->m_uiLayer->getChildByID("startpos-label"_spr));
    auto menu = pl->m_uiLayer->getChildByID("startpos-menu"_spr);
    if (!label || !menu) return;

    auto list = getStartPositions(pl);
    bool show = hackOn("startpos-switcher") && !list.empty();
    label->setVisible(show);
    menu->setVisible(show);
    if (!show) return;

    int current = currentStartPosIndex(pl, list);
    label->setString(fmt::format("{}/{}", current, list.size()).c_str());
}

// Jumps the music to the point in the song that matches a start position
static void syncMusicToStartPos(PlayLayer* pl, StartPosObject* sp) {
    pl->startMusic();
}

static void switchStartPos(int dir) {
    auto pl = PlayLayer::get();
    if (!pl) return;

    auto list = getStartPositions(pl);
    if (list.empty()) return;

    int current = currentStartPosIndex(pl, list);
    int count = static_cast<int>(list.size()) + 1;
    int next = ((current + dir) % count + count) % count;

    pl->m_startPosObject = (next == 0) ? nullptr : list[next - 1];
    pl->resetLevel();
    syncMusicToStartPos(pl, pl->m_startPosObject);
    refreshStartPosUI(pl);
}

// ---- Frame Advance ----
constexpr float STEP_DT = 1.f / 240.f;       // how much game time one step advances
constexpr float HOLD_DELAY    = 0.35f;       // seconds you hold H before it starts repeating
constexpr float HOLD_INTERVAL = 0.004f;       // seconds between steps while repeating

static bool  g_stepRequested = false;
static bool  g_hHeld         = false;
static float g_holdTimer     = 0.f;
static float g_repeatTimer   = 0.f;

// ---- Replay Bot ----
struct BotAction {
    int frame;      // physics step the input happened on
    bool down;      // press or release
    int button;
    bool player1;
};

static std::vector<BotAction> g_actions;
static size_t g_playIndex = 0;   // next action to replay
static int    g_frame = 0;       // physics steps since the attempt started
static bool   g_injecting = false;
static bool   g_resetting = false;
static std::vector<int> g_checkpointFrames;  // frame each checkpoint was placed on

static std::string replayKey(GJGameLevel* level) {
    return "replay-" + std::string(level->m_levelName);
}

static std::string serializeActions() {
    std::string out;
    for (auto const& a : g_actions) {
        out += fmt::format("{},{},{},{};", a.frame, a.down ? 1 : 0, a.button, a.player1 ? 1 : 0);
    }
    return out;
}

static void parseActions(std::string const& data) {
    g_actions.clear();
    std::istringstream stream(data);
    std::string item;
    while (std::getline(stream, item, ';')) {
        std::istringstream line(item);
        char c1, c2, c3;
        int f, d, b, p;
        if (line >> f >> c1 >> d >> c2 >> b >> c3 >> p) {
            g_actions.push_back({f, d != 0, b, p != 0});
        }
    }
}

// ---- Frame fixes: player state saved at each practice-mode respawn ----
struct PlayerFix {
    int frame;
    float x1, y1, rot1; double vy1;   // player 1
    float x2, y2, rot2; double vy2;   // player 2
};

static std::vector<PlayerFix> g_fixes;
static size_t g_fixIndex = 0;

static std::string fixKey(GJGameLevel* level) {
    return "replayfix-" + std::string(level->m_levelName);
}

static std::string serializeFixes() {
    std::string out;
    for (auto const& f : g_fixes) {
        out += fmt::format("{},{},{},{},{},{},{},{},{};",
            f.frame, f.x1, f.y1, f.rot1, f.vy1, f.x2, f.y2, f.rot2, f.vy2);
    }
    return out;
}

static void parseFixes(std::string const& data) {
    g_fixes.clear();
    std::istringstream stream(data);
    std::string item;
    while (std::getline(stream, item, ';')) {
        std::istringstream line(item);
        char c;
        PlayerFix f{};
        if (line >> f.frame >> c >> f.x1 >> c >> f.y1 >> c >> f.rot1 >> c >> f.vy1
                 >> c >> f.x2 >> c >> f.y2 >> c >> f.rot2 >> c >> f.vy2) {
            g_fixes.push_back(f);
        }
    }
}

static PlayerFix captureFix(PlayLayer* pl) {
    PlayerFix f{};
    f.frame = g_frame;
    f.x1 = pl->m_player1->getPositionX();
    f.y1 = pl->m_player1->getPositionY();
    f.rot1 = pl->m_player1->getRotation();
    f.vy1 = pl->m_player1->m_yVelocity;
    f.x2 = pl->m_player2->getPositionX();
    f.y2 = pl->m_player2->getPositionY();
    f.rot2 = pl->m_player2->getRotation();
    f.vy2 = pl->m_player2->m_yVelocity;
    return f;
}

static void applyFix(PlayLayer* pl, PlayerFix const& f) {
    auto p1 = pl->m_player1;
    auto p2 = pl->m_player2;
    if (!p1 || !p2) return;

    // Shows how far the replay had drifted from the recording at this splice
    log::info("Fix at frame {}: P1 was off by x={:.3f} y={:.3f} vy={:.3f}",
        f.frame, p1->getPositionX() - f.x1, p1->getPositionY() - f.y1, p1->m_yVelocity - f.vy1);

    p1->setPosition(ccp(f.x1, f.y1));
    p1->setRotation(f.rot1);
    p1->m_yVelocity = f.vy1;
    p2->setPosition(ccp(f.x2, f.y2));
    p2->setRotation(f.rot2);
    p2->m_yVelocity = f.vy2;
}

class $modify(HackBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        // Only freeze real levels, and only while the hack is on
        if (!hackOn("frame-advance") || static_cast<GJBaseGameLayer*>(this) != PlayLayer::get()) {
            g_stepRequested = false;
            g_hHeld = false;
            GJBaseGameLayer::update(dt);
            return;
        }

        bool step = g_stepRequested;
        g_stepRequested = false;

        // Holding H: wait a moment, then step at a steady rate
        if (g_hHeld) {
            g_holdTimer += dt;
            if (g_holdTimer >= HOLD_DELAY) {
                g_repeatTimer += dt;
                if (g_repeatTimer >= HOLD_INTERVAL) {
                    g_repeatTimer = 0.f;
                    step = true;
                }
            }
        }

        // Frozen unless a step was asked for
        if (step) GJBaseGameLayer::update(STEP_DT);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        bool inLevel = static_cast<GJBaseGameLayer*>(this) == PlayLayer::get();

        if (inLevel && g_resetting) {
            log::info("Respawn button event: down={} button={} p1={} frame={}", down, button, isPlayer1, g_frame);
        }

        // CPS counter (counts injected clicks too)
        if (inLevel && down) {
            auto now = std::chrono::steady_clock::now();
            g_clicks.push_back(now);
            g_lastClick = now;
        }

        if (inLevel && !g_injecting) {
            // The bot is driving: ignore real presses, but always let releases through
            // so the game's own respawn releases can't leave a click stuck down
            if (hackOn("bot-replay") && down) return;
            if (hackOn("bot-record")) g_actions.push_back({g_frame, down, button, isPlayer1});
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (static_cast<GJBaseGameLayer*>(this) != PlayLayer::get()) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        auto pl = PlayLayer::get();
            while (g_fixIndex < g_fixes.size() && g_fixes[g_fixIndex].frame <= g_frame) {
                if (g_fixes[g_fixIndex].frame == g_frame) applyFix(pl, g_fixes[g_fixIndex]);
                g_fixIndex++;
            }

        // Replay: fire every input that is due on this step
        if (hackOn("bot-replay")) {
            g_injecting = true;
            while (g_playIndex < g_actions.size() && g_actions[g_playIndex].frame <= g_frame) {
                auto const& a = g_actions[g_playIndex++];
                this->handleButton(a.down, a.button, a.player1);
            }
            g_injecting = false;
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_frame++;
    }
};

// ---- Opening the menu with TAB ----
class $modify(HackKeyDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_Tab) {
            toggleHackMenu();
            return true;  // consume the key
        }

        // Frame Advance: H steps one frame, hold to keep stepping
        if (key == KEY_H && !down) g_hHeld = false;  // always clear on release
        if (key == KEY_H && hackOn("frame-advance") && PlayLayer::get()) {
            if (down && !repeat) {
                g_stepRequested = true;
                g_hHeld = true;
                g_holdTimer = 0.f;
                g_repeatTimer = 0.f;
            }
            return true;  // consume H, including OS key repeats
        }

        // StartPos Switcher: Q = previous, E = next (only inside a level)
        if (down && !repeat && hackOn("startpos-switcher") && PlayLayer::get()) {
            if (key == KEY_Q) { switchStartPos(-1); return true; }
            if (key == KEY_E) { switchStartPos(1);  return true; }
        }
        
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ---- FPS + CPS Counters ----
class FpsOverlay : public CCNode {
protected:
    CCLabelBMFont* m_label = nullptr;
    CCLabelBMFont* m_cpsLabel = nullptr;
    CCDrawNode* m_dot = nullptr;
    int m_frames = 0;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();

    bool init() {
        if (!CCNode::init()) return false;

        auto winSize = CCDirector::get()->getWinSize();

        m_label = CCLabelBMFont::create("0 FPS", "bigFont.fnt");
        m_label->setScale(0.4f);
        m_label->setAnchorPoint(ccp(1.f, 1.f));
        m_label->setPosition(ccp(winSize.width - 8.f, winSize.height - 8.f));
        this->addChild(m_label);

        m_cpsLabel = CCLabelBMFont::create("0 CPS", "bigFont.fnt");
        m_cpsLabel->setScale(0.4f);
        m_cpsLabel->setAnchorPoint(ccp(1.f, 1.f));
        this->addChild(m_cpsLabel);

        m_dot = CCDrawNode::create();
        this->addChild(m_dot);

        this->scheduleUpdate();
        return true;
    }

    void update(float) override {
        auto now = std::chrono::steady_clock::now();
        auto winSize = CCDirector::get()->getWinSize();

        // ---- FPS ----
        bool fpsOn = hackOn("fps-label");
        m_label->setVisible(fpsOn);

        // Count frames over real time and refresh the number 4 times a second
        m_frames++;
        float elapsed = std::chrono::duration<float>(now - m_start).count();
        if (elapsed >= 0.25f) {
            int fps = static_cast<int>(m_frames / elapsed + 0.5f);
            m_label->setString(fmt::format("{} FPS", fps).c_str());
            m_frames = 0;
            m_start = now;
        }

        // ---- CPS ----
        m_cpsLabel->setVisible(hackOn("cps-label"));

        // Forget clicks older than one second
        while (!g_clicks.empty() && now - g_clicks.front() > std::chrono::seconds(1)) {
            g_clicks.pop_front();
        }
        m_cpsLabel->setString(fmt::format("{} CPS", g_clicks.size()).c_str());

        // Flash: green right after a click, fading back to white over 0.2 seconds
        float sinceClick = std::chrono::duration<float>(now - g_lastClick).count();
        float k = std::min(sinceClick / 0.2f, 1.f);
        GLubyte other = static_cast<GLubyte>(255.f * k);
        m_cpsLabel->setColor(ccColor3B{other, 255, other});

        // Sits just below the FPS line, or moves up into its spot if FPS is off
        float y = winSize.height - 8.f - (fpsOn ? 16.f : 0.f);
        m_cpsLabel->setPosition(ccp(winSize.width - 8.f, y));

        // ---- Cheat Indicator ----
        m_dot->clear();
        if (hackOn("cheat-indicator")) {
            bool unsafe = hackOn("safe-mode") ||
                (hackOn("auto-safe-mode") && (g_badHackUsed || badHackActive()));

            ccColor4F color = unsafe ? ccColor4F{1.f, 0.15f, 0.15f, 1.f}
                                     : ccColor4F{0.2f, 1.f, 0.2f, 1.f};

            constexpr int SEGMENTS = 20;
            CCPoint verts[SEGMENTS];
            for (int i = 0; i < SEGMENTS; i++) {
                float angle = (static_cast<float>(i) / SEGMENTS) * 2.f * static_cast<float>(M_PI);
                verts[i] = ccp(18.f + std::cos(angle) * 7.f, 18.f + std::sin(angle) * 7.f);
            }
            m_dot->drawPolygon(verts, SEGMENTS, color, 0.f, color);
        }
    }

public:
    static FpsOverlay* create() {
        auto ret = new FpsOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---- .violet replay files ----
static std::string g_lastReplayName;  // level of the most recent recorded/loaded replay

static std::string safeFileName(std::string name) {
    for (auto& c : name) {
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    }
    return name.empty() ? "replay" : name;
}

static void saveReplayFile() {
    if (g_lastReplayName.empty()) {
        Notification::create("Nothing to save yet: record and finish a level first",
                             NotificationIcon::Warning)->show();
        return;
    }

    auto name = g_lastReplayName;
    auto actions = Mod::get()->getSavedValue<std::string>("replay-" + name, "");
    auto fixes = Mod::get()->getSavedValue<std::string>("replayfix-" + name, "");
    std::string text = fmt::format("violet-replay 1\nlevel={}\nactions={}\nfixes={}\n",
                                   name, actions, fixes);

    file::FilePickOptions options;
    options.defaultPath = Mod::get()->getSaveDir() / (safeFileName(name) + ".violet");
    options.filters.push_back({"Violet macro", {"*.violet"}});

    async::spawn(file::pick(file::PickMode::SaveFile, options),
        [text](geode::Result<std::optional<asp::fs::Path>> result) {
        if (!result.isOk()) return;  // failed

        auto picked = result.unwrap();
        if (!picked) return;  // dialog cancelled

        auto written = file::writeString(*picked, text);
        if (written.isErr()) {
            Notification::create("Couldn't write the file", NotificationIcon::Error)->show();
        } else {
            Notification::create("Replay saved", NotificationIcon::Success)->show();
        }
    });
}

static void loadReplayFile() {
    file::FilePickOptions options;
    options.defaultPath = Mod::get()->getSaveDir();
    options.filters.push_back({"Violet macro", {"*.violet"}});

    async::spawn(file::pick(file::PickMode::OpenFile, options),
        [](geode::Result<std::optional<asp::fs::Path>> result) {
        if (!result.isOk()) return;  // failed

        auto picked = result.unwrap();
        if (!picked) return;  // dialog cancelled

        auto read = file::readString(*picked);
        if (read.isErr()) {
            Notification::create("Couldn't read the file", NotificationIcon::Error)->show();
            return;
        }

        std::istringstream stream(read.unwrap());
        std::string magic, line, name, actions, fixes;
        std::getline(stream, magic);
        if (magic.rfind("violet-replay", 0) != 0) {
            Notification::create("Not a valid .violet file", NotificationIcon::Error)->show();
            return;
        }

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("level=", 0) == 0)        name = line.substr(6);
            else if (line.rfind("actions=", 0) == 0) actions = line.substr(8);
            else if (line.rfind("fixes=", 0) == 0)   fixes = line.substr(6);
        }

        if (name.empty() || actions.empty()) {
            Notification::create("That file has no replay data", NotificationIcon::Error)->show();
            return;
        }

        Mod::get()->setSavedValue<std::string>("replay-" + name, actions);
        Mod::get()->setSavedValue<std::string>("replayfix-" + name, fixes);
        g_lastReplayName = name;

        Notification::create(fmt::format("Loaded replay for {}", name),
                             NotificationIcon::Success)->show();
    });
}

// ---- Noclip + Show Hitboxes ----
class $modify(HackPlayLayer, PlayLayer) {
    struct Fields {
        int frames = 0;
        bool completed = false;
        bool badHackUsed = false;
    };

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        // When noclip is on, never run the death logic
        if (hackOn("noclip") && obj != m_anticheatSpike) return;

        bool wasDead = m_player1->m_isDead;
        PlayLayer::destroyPlayer(player, obj);

        // Ignore repeat calls and the anti-cheat spike
        if (wasDead || obj == m_anticheatSpike) return;
        if (!m_player1->m_isDead) return;

        if (hackOn("jumpscare") && rollJumpscare()) this->showJumpscare();
    }

    void showJumpscare() {
        auto scene = CCDirector::get()->getRunningScene();

        // Don't stack a second jumpscare on top of one that's still playing
        if (scene->getChildByID("jumpscare-sprite"_spr)) return;

        auto winSize = CCDirector::get()->getWinSize();

        auto sprite = CCSprite::create("speed.jpg"_spr);
        if (!sprite) return;

        sprite->setID("jumpscare-sprite"_spr);
        sprite->setPosition(ccp(winSize.width / 2, winSize.height / 2));
        sprite->setScale(std::max(
            winSize.width / sprite->getContentWidth(),
            winSize.height / sprite->getContentHeight()
        ));
        scene->addChild(sprite, 9999);

        sprite->runAction(CCSequence::create(
            CCDelayTime::create(JUMPSCARE_HOLD),
            CCFadeOut::create(JUMPSCARE_FADE),
            CCRemoveSelf::create(),
            nullptr
        ));

        FMODAudioEngine::sharedEngine()->playEffect("anger.mp3"_spr);
    }

     void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        this->updateHitboxes();
        this->updateStartPosUI();

        // Instant Complete: wait a few frames for the level to finish loading, then finish it once
        if (hackOn("instant-complete") && !m_fields->completed) {
            if (++m_fields->frames >= 10) {
                m_fields->completed = true;
                this->levelComplete();
            }
        }

        if (badHackActive()) { m_fields->badHackUsed = true; g_badHackUsed = true; }
    }

    void levelComplete() {
        if (hackOn("bot-record") && !hackOn("bot-replay")) {
            Mod::get()->setSavedValue<std::string>(replayKey(m_level), serializeActions());
            Mod::get()->setSavedValue<std::string>(fixKey(m_level), serializeFixes());
            g_lastReplayName = std::string(m_level->m_levelName);
            log::info("Replay saved: {} inputs, {} frames", g_actions.size(), g_frame);
        }

        bool safe = hackOn("safe-mode") ||
            (hackOn("auto-safe-mode") && (m_fields->badHackUsed || badHackActive()));
        g_safeCompletion = safe;

        if (safe) {
            // Run the completion as a test run so nothing saves or submits
            bool oldTestMode = m_isTestMode;
            m_isTestMode = true;
            PlayLayer::levelComplete();
            m_isTestMode = oldTestMode;
            return;
        }
        PlayLayer::levelComplete();
    }

    CheckpointObject* createCheckpoint() {
        auto cp = PlayLayer::createCheckpoint();
        if (cp) g_checkpointFrames.push_back(g_frame);
        return cp;
    }

    void updateStartPosUI() {
        if (!m_uiLayer) return;

        // Build the UI the first time we get here in a level
        if (!m_uiLayer->getChildByID("startpos-label"_spr)) {
            this->buildStartPosUI();
            refreshStartPosUI(this);
        }

        // Refresh when the toggle is switched on or off
        static bool wasOn = false;
        bool on = hackOn("startpos-switcher");
        if (on != wasOn) {
            refreshStartPosUI(this);
            wasOn = on;
        }
    }

    void buildStartPosUI() {
        auto winSize = CCDirector::get()->getWinSize();
        float y = 25.f;  // height above the bottom of the screen

        auto label = CCLabelBMFont::create("0/0", "bigFont.fnt");
        label->setID("startpos-label"_spr);
        label->setScale(0.5f);
        label->setPosition(ccp(winSize.width / 2.f, y));
        m_uiLayer->addChild(label, 100);

        auto menu = CCMenu::create();
        menu->setID("startpos-menu"_spr);
        menu->setPosition(ccp(winSize.width / 2.f, y));

        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        leftSpr->setScale(0.5f);
        auto leftBtn = CCMenuItemSpriteExtra::create(
            leftSpr, this, menu_selector(HackPlayLayer::onStartPosPrev)
        );
        leftBtn->setPosition(ccp(-45.f, 0.f));
        menu->addChild(leftBtn);

        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setScale(0.5f);
        rightSpr->setFlipX(true);
        auto rightBtn = CCMenuItemSpriteExtra::create(
            rightSpr, this, menu_selector(HackPlayLayer::onStartPosNext)
        );
        rightBtn->setPosition(ccp(45.f, 0.f));
        menu->addChild(rightBtn);

        m_uiLayer->addChild(menu, 100);
    }

    void onStartPosPrev(CCObject*) { switchStartPos(-1); }
    void onStartPosNext(CCObject*) { switchStartPos(1); }

    void updateHitboxes() {
        if (!m_objectLayer) return;

        // Find our draw node, or create it on first use
        auto node = static_cast<CCDrawNode*>(m_objectLayer->getChildByID("hitbox-node"_spr));
        if (!node) {
            node = CCDrawNode::create();
            node->setID("hitbox-node"_spr);
            m_objectLayer->addChild(node, 1000);
        }

        node->clear();

        if (!hackOn("show-hitboxes")) {
            node->setVisible(false);
            return;
        }
        node->setVisible(true);

        const ccColor4F playerColor   {1.f, 1.f, 0.f, 1.f};  // yellow
        const ccColor4F hazardColor   {1.f, 0.f, 0.f, 1.f};  // red
        const ccColor4F solidColor    {0.f, 0.4f, 1.f, 1.f}; // blue
        const ccColor4F interactColor {0.f, 1.f, 0.f, 1.f};  // green

        // Players
        if (m_player1) drawBox(node, m_player1->getObjectRect(), playerColor);
        if (m_player2 && m_player2->isVisible()) {
            drawBox(node, m_player2->getObjectRect(), playerColor);
        }

        // Objects: only the ones near the player, to keep big levels fast
        float px = m_player1 ? m_player1->getPositionX() : 0.f;
        float range = 600.f;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            if (obj->m_isGroupDisabled) continue;

            auto type = obj->m_objectType;
            if (type == GameObjectType::Decoration) continue;
            if (type == GameObjectType::Modifier) continue;  // triggers

            if (std::abs(obj->getPositionX() - px) > range) continue;

            ccColor4F color = interactColor;
            if (type == GameObjectType::Hazard) color = hazardColor;
            else if (type == GameObjectType::Solid) color = solidColor;

            if (type == GameObjectType::Slope) {
                drawSlope(node, obj, solidColor);
                continue;
            }

            if (obj->m_objectRadius > 0.f) {
                // Round object (saw, orb, etc.)
                auto rect = obj->getObjectRect();
                CCPoint center = ccp(rect.getMidX(), rect.getMidY());
                float radius = obj->m_objectRadius * obj->getScaleX();
                drawCircle(node, center, radius, color);
            } 
            else 
            {
                // Is the object rotated by something other than a multiple of 90 degrees?
                float rot = std::fmod(std::abs(obj->getRotation()), 90.f);
                bool tilted = rot > 0.5f && rot < 89.5f;

                if (tilted && obj->m_orientedBox) {
                    obj->updateOrientedBox();  // make sure the corners are current
                    auto box = obj->m_orientedBox;
                    drawQuad(node, box->m_corners[0], box->m_corners[1], box->m_corners[2], box->m_corners[3], color);
                }
                else 
                {
                    drawBox(node, obj->getObjectRect(), color);
                }
            }
        }
    }

    bool init(GJGameLevel* level, bool dontCreateObjects) {
    if (!PlayLayer::init(level, dontCreateObjects)) return false;

        if (auto overlay = FpsOverlay::create()) {
            m_uiLayer->addChild(overlay, 100);
        }

        g_frame = 0;
        g_checkpointFrames.clear();
        g_playIndex = 0;
        g_fixes.clear();
        g_fixIndex = 0;
        return true;
    }

    void resetLevel() {
        // Bot: decide where this attempt rewinds to BEFORE the game resets anything
        int cpCount = m_checkpointArray ? m_checkpointArray->count() : 0;
        bool fromCheckpoint = m_isPracticeMode && cpCount > 0 && !g_checkpointFrames.empty();

        if (fromCheckpoint) {
            if (g_checkpointFrames.size() > static_cast<size_t>(cpCount)) {
                g_checkpointFrames.resize(cpCount);
            }
            g_frame = g_checkpointFrames.back();
        } else {
            g_frame = 0;
            g_checkpointFrames.clear();
        }

        // Trim the recording now, so anything the game does during the reset
        // (like releasing held buttons) gets recorded at the right frame
        if (hackOn("bot-record") && !hackOn("bot-replay")) {
            std::erase_if(g_actions, [](BotAction const& a) { return a.frame >= g_frame; });
            std::erase_if(g_fixes, [](PlayerFix const& f) { return f.frame >= g_frame; });
        }

        g_resetting = true;
        PlayLayer::resetLevel();
        g_resetting = false;

        m_fields->badHackUsed = false;
        g_badHackUsed = false;

        if (hackOn("bot-replay")) {
            parseActions(Mod::get()->getSavedValue<std::string>(replayKey(m_level), ""));
            parseFixes(Mod::get()->getSavedValue<std::string>(fixKey(m_level), ""));
            g_playIndex = 0;
            while (g_playIndex < g_actions.size() && g_actions[g_playIndex].frame < g_frame) {
                g_playIndex++;
            }
            g_fixIndex = 0;
            while (g_fixIndex < g_fixes.size() && g_fixes[g_fixIndex].frame < g_frame) {
                g_fixIndex++;
            }
        } else if (hackOn("bot-record") && fromCheckpoint) {
            // Players are now at the restored checkpoint state, so capture them
            g_fixes.push_back(captureFix(this));
        }

        log::info("Bot reset: practice={} checkpoints={} listed={} frame={}",
                  m_isPracticeMode, cpCount, g_checkpointFrames.size(), g_frame);
    }
};

// ---- Icons: report everything as unlocked ----
class $modify(HackGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (hackOn("icons")) return true;
        return GameManager::isIconUnlocked(id, type);
    }

    bool isColorUnlocked(int id, UnlockType type) {
        if (hackOn("icons")) return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

class $modify(HackStatsManager, GameStatsManager) {
    bool isItemUnlocked(UnlockType type, int id) {
        if (hackOn("icons")) return true;
        return GameStatsManager::isItemUnlocked(type, id);
    }
};

// ---- No Wave Pulse ----
class $modify(HackHardStreak, HardStreak) {
    void updateStroke(float dt) {
        if (hackOn("no-wave-pulse")) m_pulseSize = 1.f;  // pin the trail width
        HardStreak::updateStroke(dt);
    }
};

// ---- No Trail ----
class $modify(HackPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        // Remember the last state so we can bring the trail back when the toggle goes off
        static bool wasOn = false;
        bool on = hackOn("no-trail");

        if (on || wasOn) {
            if (m_regularTrail) m_regularTrail->setVisible(!on);
            if (m_shipStreak)   m_shipStreak->setVisible(!on);
        }
        wasOn = on;
    }

    void playSpawnEffect() {
        // Skip the respawn flash when the toggle is on
        if (hackOn("no-respawn-flash")) return;
        PlayerObject::playSpawnEffect();
    }
};

// ---- No Particles ----
class $modify(HackParticleQuad, CCParticleSystemQuad) {
    void draw() {
        // Inside a level with the toggle on, skip drawing the particles
        if (hackOn("no-particles") && PlayLayer::get()) return;
        CCParticleSystemQuad::draw();
    }
};

// ---- Verify Hack ----
class $modify(HackEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (hackOn("verify-hack") && level) level->m_isVerified = true;
        return EditLevelLayer::init(level);
    }
};

class $modify(HackLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (hackOn("verify-hack") && level) level->m_isVerified = true;
        if (hackOn("copy-hack") && level) level->m_password = 1;

        if (!LevelInfoLayer::init(level, challenge)) return false;

        // Make sure the copy button is showing
        if (hackOn("copy-hack") && m_cloneBtn) m_cloneBtn->setVisible(true);
        return true;
    }
};

// ---- Safe Mode label on the end screen ----
class $modify(HackEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();

        if (g_safeCompletion && m_mainLayer) {
            auto winSize = CCDirector::get()->getWinSize();

            auto label = CCLabelBMFont::create("safe mode bro", "bigFont.fnt");
            label->setScale(0.4f);
            label->setPosition(ccp(winSize.width / 2.f, winSize.height / 2.f - 95.f));
            m_mainLayer->addChild(label);
        }
    }
};