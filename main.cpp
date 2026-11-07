#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

using namespace geode::prelude;

// ---- Speedhack range ----
constexpr float SPEED_MIN = 0.1f;
constexpr float SPEED_MAX = 3.0f;

// ---- Slider layout (offsets from the bottom of the popup) ----
constexpr float SLIDER_Y = 45.f;
constexpr float LABEL_Y  = 20.f;
constexpr float EXTRA_HEIGHT = 90.f;  // space reserved at the bottom for the slider

// ---- Hack list: add a new line here to get a new toggle ----
struct HackEntry {
    const char* id;
    const char* label;
};

static const std::array<HackEntry, 3> HACKS = {{
    {"noclip",        "Noclip"},
    {"show-hitboxes", "Show Hitboxes"},
    {"speedhack",     "Speedhack"},
}};

// Use this from any hook: if (hackOn("noclip")) { ... }
static bool hackOn(std::string const& id) {
    return Mod::get()->getSavedValue<bool>(id, false);
}

static float getSpeed() {
    return Mod::get()->getSavedValue<float>("speedhack-value", 1.0f);
}

// ---- Speedhack: on = slider speed, off = normal 1x (read every frame) ----
class $modify(HackScheduler, CCScheduler) {
    void update(float dt) {
        float speed = hackOn("speedhack") ? getSpeed() : 1.0f;
        CCScheduler::update(dt * speed);
    }
};

// ---- The popup ----
class HackMenu : public geode::Popup {
protected:
    CCLabelBMFont* m_speedLabel = nullptr;

    bool init() {
        if (!Popup::init(300.f, 60.f + HACKS.size() * 40.f + EXTRA_HEIGHT)) return false;

        this->setTitle("Violet's Mod Menu");

        float top = m_size.height - 55.f;
        for (size_t i = 0; i < HACKS.size(); i++) {
            float y = top - i * 40.f;

            auto toggler = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(HackMenu::onToggle), 0.7f
            );
            toggler->setTag(static_cast<int>(i));
            toggler->toggle(hackOn(HACKS[i].id));
            toggler->setPosition(ccp(40.f, y));
            m_buttonMenu->addChild(toggler);

            auto label = CCLabelBMFont::create(HACKS[i].label, "bigFont.fnt");
            label->setScale(0.6f);
            label->setAnchorPoint(ccp(0.f, 0.5f));
            label->setPosition(ccp(70.f, y));
            m_buttonMenu->addChild(label);
        }

        // Speed slider
        auto slider = Slider::create(this, menu_selector(HackMenu::onSpeedSlider), 0.8f);
        slider->setValue((getSpeed() - SPEED_MIN) / (SPEED_MAX - SPEED_MIN));
        m_mainLayer->addChildAtPosition(slider, Anchor::Bottom, ccp(0.f, SLIDER_Y));

        m_speedLabel = CCLabelBMFont::create(fmt::format("{:.2f}x", getSpeed()).c_str(), "bigFont.fnt");
        m_speedLabel->setScale(0.5f);
        m_mainLayer->addChildAtPosition(m_speedLabel, Anchor::Bottom, ccp(0.f, LABEL_Y));

        s_open = this;
        return true;
    }

    void onToggle(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        auto const& hack = HACKS[toggler->getTag()];
        // The callback fires before the visual state flips, hence the "!"
        Mod::get()->setSavedValue<bool>(hack.id, !toggler->isToggled());
    }

    void onSpeedSlider(CCObject* sender) {
        float value = static_cast<SliderThumb*>(sender)->getValue();  // 0 to 1
        float speed = SPEED_MIN + value * (SPEED_MAX - SPEED_MIN);

        Mod::get()->setSavedValue<float>("speedhack-value", speed);
        m_speedLabel->setString(fmt::format("{:.2f}x", speed).c_str());
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

// ---- Opening the menu with TAB ----
class $modify(HackKeyDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat && key == KEY_Tab) {
            toggleHackMenu();
            return true;  // consume the key
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ---- Noclip + Show Hitboxes ----
class $modify(HackPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        // When noclip is on, never run the death logic
        if (hackOn("noclip") && obj != m_anticheatSpike) return;

        PlayLayer::destroyPlayer(player, obj);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        this->updateHitboxes();
    }

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

            auto type = obj->m_objectType;
            if (type == GameObjectType::Decoration) continue;
            if (type == GameObjectType::Modifier) continue;  // triggers

            if (std::abs(obj->getPositionX() - px) > range) continue;

            ccColor4F color = interactColor;
            if (type == GameObjectType::Hazard) color = hazardColor;
            else if (type == GameObjectType::Solid) color = solidColor;

            drawBox(node, obj->getObjectRect(), color);
        }
    }
};