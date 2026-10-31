#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>

using namespace geode::prelude;

namespace {
	using Clock = std::chrono::steady_clock;

	struct State {
		Clock::time_point lastClick = Clock::time_point::min();
		Clock::time_point lastSave = Clock::time_point::min();
		bool clicking = false;
		int saves = 0;
		bool assisted = false;
	};
	State g;

	long long msSince(Clock::time_point t) {
		if (t == Clock::time_point::min()) return 1000000;
		return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
	}

	std::string getMode() {
		return Mod::get()->getSettingValue<std::string>("mode");
	}
}

// Registra los clicks (funciona para todos los vehículos y modo dual)
class $modify(CFBaseGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		if (down) {
			g.lastClick = Clock::now();
			g.clicking = true;
		} else {
			g.clicking = false;
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}
};

class $modify(CFPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		g.saves = 0;
		g.assisted = false;
		g.lastSave = Clock::time_point::min();
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		// Nunca cancelamos el spike anti-cheat del juego
		if (obj != nullptr && obj == m_anticheatSpike) {
			return PlayLayer::destroyPlayer(player, obj);
		}

		auto mode = getMode();
		if (mode == "Apagado") {
			return PlayLayer::destroyPlayer(player, obj);
		}

		bool shouldSave = false;
		bool newSaveEvent = msSince(g.lastSave) > 200; // evita contar cada frame

		if (mode == "Agresivo") {
			bool requireClick = Mod::get()->getSettingValue<bool>("aggressive-require-click");
			shouldSave = !requireClick || g.clicking || msSince(g.lastClick) <= 600;
		} else { // Balanceado
			auto window = Mod::get()->getSettingValue<int64_t>("balanced-window");
			auto maxSaves = Mod::get()->getSettingValue<int64_t>("balanced-max-saves");
			shouldSave = msSince(g.lastClick) <= window;
			if (shouldSave && newSaveEvent && g.saves >= maxSaves) {
				shouldSave = false;
			}
		}

		if (!shouldSave) {
			return PlayLayer::destroyPlayer(player, obj);
		}

		if (newSaveEvent) g.saves++;
		g.lastSave = Clock::now();
		g.assisted = true;
		// No llamamos al original: el jugador sobrevive este golpe
	}

	void levelComplete() {
		if (g.assisted && Mod::get()->getSettingValue<bool>("safe-mode")) {
			FLAlertLayer::create(
				"Click Fix",
				"Este intento fue <cy>asistido</c>, así que no cuenta como completado.\n"
				"Desactiva el <cg>Modo seguro</c> en los ajustes del mod si quieres lo contrario.",
				"OK"
			)->show();
			this->resetLevel();
			return;
		}
		PlayLayer::levelComplete();
	}
};

// Botón en el menú de pausa para cambiar de modo rápido
class $modify(CFPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto menu = this->getChildByID("right-button-menu");
		if (!menu) return;

		auto spr = ButtonSprite::create("Click Fix", "bigFont.fnt", "GJ_button_01.png", 0.6f);
		spr->setScale(0.6f);
		auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CFPauseLayer::onClickFix));
		btn->setID("click-fix-button"_spr);
		menu->addChild(btn);
		menu->updateLayout();
	}

	void onClickFix(CCObject*) {
		auto mode = getMode();
		std::string next = "Balanceado";
		if (mode == "Apagado") next = "Balanceado";
		else if (mode == "Balanceado") next = "Agresivo";
		else next = "Apagado";

		Mod::get()->setSettingValue<std::string>("mode", next);
		Notification::create("Click Fix: " + next, NotificationIcon::Success, 1.5f)->show();
	}
};
