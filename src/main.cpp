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

		// Prediccion (Principal)
		bool simulating = false;     // estamos simulando hacia adelante
		bool simDied = false;        // la simulacion termino en muerte
		int internalCalls = 0;       // clicks que mete el propio mod
		int corrections = 0;         // correcciones en este intento
		bool correcting = false;     // estamos en medio de una correccion
	};
	State g;

	long long msSince(Clock::time_point t) {
		if (t == Clock::time_point::min()) return 1000000;
		return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
	}

	std::string getMode() {
		return Mod::get()->getSettingValue<std::string>("mode");
	}

	std::string getPrincipalMode() {
		return Mod::get()->getSettingValue<std::string>("principal-mode");
	}

	bool isHolding(PlayerObject* p) {
		if (!p) return false;
		auto it = p->m_holdingButtons.find(1);
		return it != p->m_holdingButtons.end() && it->second;
	}
}

class $modify(CFBaseGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		// Solo contamos los clicks reales del jugador, no los del mod
		if (g.internalCalls == 0 && !g.simulating && button == 1) {
			if (down) {
				g.lastClick = Clock::now();
				g.clicking = true;
			} else {
				g.clicking = false;
			}
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	// Mete un click propio sin que cuente como click del jugador
	void modClick(bool down) {
		g.internalCalls++;
		this->handleButton(down, 1, true);
		g.internalCalls--;
	}

	// Simula "frames" pasos con el click en "hold". Devuelve true si sobrevives.
	bool simulate(PlayLayer* pl, CheckpointObject* cp, bool hold, int frames, float dt) {
		g.simulating = true;
		g.simDied = false;

		this->modClick(hold);
		for (int i = 0; i < frames && !g.simDied; i++) {
			GJBaseGameLayer::update(dt);
		}

		bool ok = !g.simDied;
		g.simulating = false;
		g.simDied = false;

		// volvemos al estado real
		pl->loadFromCheckpoint(cp);
		return ok;
	}

	void update(float dt) {
		auto pl = PlayLayer::get();
		auto self = static_cast<GJBaseGameLayer*>(this);

		if (!pl || static_cast<GJBaseGameLayer*>(pl) != self || g.simulating) {
			return GJBaseGameLayer::update(dt);
		}

		auto pmode = getPrincipalMode();
		if (pmode == "Apagado" || dt <= 0.f || dt > 0.1f ||
			!m_started || !m_player1 || m_player1->m_isDead || m_levelEndAnimationStarted) {
			return GJBaseGameLayer::update(dt);
		}

		bool aggressive = (pmode == "Agresivo");
		int frames = aggressive
			? static_cast<int>(Mod::get()->getSettingValue<int64_t>("principal-aggressive-frames"))
			: static_cast<int>(Mod::get()->getSettingValue<int64_t>("principal-balanced-frames"));

		// Lo que el jugador quiere en este momento
		bool intent = g.clicking;
		bool current = isHolding(m_player1);

		auto cp = pl->createCheckpoint();
		if (!cp) {
			return GJBaseGameLayer::update(dt);
		}
		cp->retain();

		bool desired = intent;

		// 1) lo que quiere el jugador, sobrevive?
		bool intentOk = this->simulate(pl, cp, intent, frames, dt);
		if (!intentOk) {
			// 2) lo contrario, sobrevive?
			bool altOk = this->simulate(pl, cp, !intent, frames, dt);
			if (altOk) {
				bool allowed = true;
				if (!aggressive) {
					auto maxC = static_cast<int>(Mod::get()->getSettingValue<int64_t>("principal-balanced-max"));
					if (!g.correcting && g.corrections >= maxC) {
						allowed = false;
					}
				}
				if (allowed) {
					if (!g.correcting) {
						g.corrections++;
						g.correcting = true;
					}
					desired = !intent;
					g.assisted = true;
				}
			}
		} else {
			g.correcting = false;
		}

		cp->release();

		// Aplicamos la decision real si es distinta al estado actual
		if (desired != current) {
			this->modClick(desired);
		}

		GJBaseGameLayer::update(dt);
	}
};

class $modify(CFPlayLayer, PlayLayer) {
	void resetLevel() {
		if (g.simulating) return;
		PlayLayer::resetLevel();
		g.saves = 0;
		g.assisted = false;
		g.lastSave = Clock::time_point::min();
		g.corrections = 0;
		g.correcting = false;
	}

	void destroyPlayer(PlayerObject* player, GameObject* obj) {
		// Durante la simulacion solo anotamos que ibamos a morir
		if (g.simulating) {
			g.simDied = true;
			return;
		}

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
		if (g.simulating) return;
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

// Botón en el menú de pausa para cambiar el modo Principal rápido
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
		auto mode = getPrincipalMode();
		std::string next = "Balanceado";
		if (mode == "Apagado") next = "Balanceado";
		else if (mode == "Balanceado") next = "Agresivo";
		else next = "Apagado";

		Mod::get()->setSettingValue<std::string>("principal-mode", next);
		Notification::create("Click Fix Principal: " + next, NotificationIcon::Success, 1.5f)->show();
	}
};
