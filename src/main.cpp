#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace geode::prelude;

namespace {
	using Clock = std::chrono::steady_clock;

	enum Level { LvOff = 0, LvBalanced = 1, LvAggressive = 2 };

	long long msSince(Clock::time_point t) {
		if (t == Clock::time_point::min()) return 1000000;
		return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
	}

	// Ajustes en cache: antes se leian como strings en CADA frame.
	struct Config {
		int principal = LvOff;
		int secondary = LvOff;
		int principalBalancedMs = 60;
		int principalBalancedMax = 5;
		int principalAggressiveMs = 120;
		int clickWindowMs = 150;
		int balancedWindow = 120;
		int balancedMaxSaves = 3;
		bool aggressiveRequireClick = true;
		bool safeMode = true;
	};
	Config cfg;
	Clock::time_point cfgStamp = Clock::time_point::min();

	int parseLevel(std::string const& s) {
		if (s == "Balanceado") return LvBalanced;
		if (s == "Agresivo") return LvAggressive;
		return LvOff;
	}

	void refreshConfig() {
		auto mod = Mod::get();
		cfg.principal = parseLevel(mod->getSettingValue<std::string>("principal-mode"));
		cfg.secondary = parseLevel(mod->getSettingValue<std::string>("mode"));
		cfg.principalBalancedMs = static_cast<int>(mod->getSettingValue<int64_t>("principal-balanced-ms"));
		cfg.principalBalancedMax = static_cast<int>(mod->getSettingValue<int64_t>("principal-balanced-max"));
		cfg.principalAggressiveMs = static_cast<int>(mod->getSettingValue<int64_t>("principal-aggressive-ms"));
		cfg.clickWindowMs = static_cast<int>(mod->getSettingValue<int64_t>("principal-click-window-ms"));
		cfg.balancedWindow = static_cast<int>(mod->getSettingValue<int64_t>("balanced-window"));
		cfg.balancedMaxSaves = static_cast<int>(mod->getSettingValue<int64_t>("balanced-max-saves"));
		cfg.aggressiveRequireClick = mod->getSettingValue<bool>("aggressive-require-click");
		cfg.safeMode = mod->getSettingValue<bool>("safe-mode");
		cfgStamp = Clock::now();
	}

	void ensureConfig() {
		if (msSince(cfgStamp) > 500) refreshConfig();
	}

	struct State {
		Clock::time_point lastClick = Clock::time_point::min();
		Clock::time_point lastRelease = Clock::time_point::min();
		Clock::time_point lastSave = Clock::time_point::min();
		Clock::time_point lastEval = Clock::time_point::min();
		bool clicking = false;
		int saves = 0;
		bool assisted = false;

		// Prediccion (Principal)
		bool simulating = false;   // estamos simulando hacia adelante
		bool simDied = false;      // la simulacion termino en muerte
		int internalCalls = 0;     // clicks que mete el propio mod
		int corrections = 0;       // correcciones en este intento
		bool overriding = false;   // el mod esta invirtiendo tu click ahora mismo
		bool wasOverriding = false;
		bool intentDirty = true;   // el jugador cambio de click: evaluar ya

		// Control de costo: la prediccion se auto-limita segun cuanto tarda
		double costEmaMs = 1.0;
		double intervalMs = 16.0;
	};
	State g;

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
				g.lastRelease = Clock::now();
				g.clicking = false;
			}
			g.intentDirty = true;
		}
		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	// Mete un click propio sin que cuente como click del jugador
	void modClick(bool down) {
		g.internalCalls++;
		this->handleButton(down, 1, true);
		g.internalCalls--;
	}

	// Devuelve el control al jugador SIN inventar un click nuevo:
	// si el jugador sigue manteniendo, solo se corrige el estado del boton en silencio
	// (antes se mandaba un "press" falso y eso causaba saltos extra).
	void endOverride(bool intentHold) {
		if (!m_player1 || isHolding(m_player1) == intentHold) return;
		if (intentHold) {
			m_player1->m_holdingButtons[1] = true;
		} else {
			this->modClick(false);
		}
	}

	// Simula "calls" pasos. inverted = false: el jugador sigue como va. inverted = true: click contrario.
	// Devuelve cuantos pasos sobrevives (calls = sobrevives todo).
	int simulate(PlayLayer* pl, CheckpointObject* cp, bool intent, bool inverted, int calls, float simDt) {
		g.simulating = true;
		g.simDied = false;

		bool wantHold = inverted ? !intent : intent;
		if (isHolding(m_player1) != wantHold) {
			this->modClick(wantHold);
		}

		int survived = calls;
		for (int i = 0; i < calls; i++) {
			GJBaseGameLayer::update(simDt);
			if (g.simDied) {
				survived = i;
				break;
			}
		}

		g.simulating = false;
		g.simDied = false;

		// volvemos al estado real
		pl->loadFromCheckpoint(cp);
		return survived;
	}

	// Decide si hay que invertir el click del jugador (se llama de vez en cuando, no cada frame)
	void evaluate(PlayLayer* pl, bool intent, bool aggressive, float dt) {
		auto t0 = Clock::now();

		// Se simula en pasos de ~16 ms (la fisica del juego es de paso fijo), asi cuesta
		// mucho menos que simular frame por frame a 240 FPS. El horizonte esta en ms.
		float simDt = std::max(dt, 1.f / 60.f);
		int lookMs = aggressive ? cfg.principalAggressiveMs : cfg.principalBalancedMs;
		int calls = std::clamp(
			static_cast<int>(std::ceil(static_cast<float>(lookMs) / (simDt * 1000.f))), 1, 24
		);

		// Estado real del input: la simulacion lo consume/ensucia, hay que dejarlo como estaba
		bool before = isHolding(m_player1);
		auto savedQueue = m_queuedButtons;
		auto restoreInput = [&]() {
			m_queuedButtons = savedQueue;
			if (m_player1) m_player1->m_holdingButtons[1] = before;
		};

		auto cp = pl->createCheckpoint();
		if (!cp) return;
		cp->retain();

		bool wantOverride = false;

		// 1) lo que quiere el jugador, cuanto sobrevive?
		int a = this->simulate(pl, cp, intent, false, calls, simDt);
		restoreInput();
		if (a < calls) {
			// 2) lo contrario, cuanto sobrevive?
			int b = this->simulate(pl, cp, intent, true, calls, simDt);
			restoreInput();
			if (b >= calls) {
				wantOverride = true;            // la alternativa te salva por completo
			} else if (aggressive && b >= a + 2) {
				wantOverride = true;            // no te salva, pero aguantas mas (gana tiempo)
			}
		}

		cp->release();

		if (wantOverride && !g.overriding && !aggressive &&
			g.corrections >= cfg.principalBalancedMax) {
			wantOverride = false;
		}
		if (wantOverride && !g.overriding) g.corrections++;
		if (wantOverride) g.assisted = true;
		g.overriding = wantOverride;

		// Auto-limite: cuanto mas caro sale evaluar, mas espaciadas son las evaluaciones
		double costMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		g.costEmaMs = g.costEmaMs * 0.7 + costMs * 0.3;
		g.intervalMs = std::clamp(g.costEmaMs * 5.0, 16.0, 150.0);
	}

	void update(float dt) {
		auto pl = PlayLayer::get();
		auto self = static_cast<GJBaseGameLayer*>(this);

		if (!pl || static_cast<GJBaseGameLayer*>(pl) != self || g.simulating) {
			return GJBaseGameLayer::update(dt);
		}

		ensureConfig();

		bool inactive = cfg.principal == LvOff || dt <= 0.f || dt > 0.1f ||
			!m_started || !m_player1 || m_player1->m_isDead || m_levelEndAnimationStarted;

		// Natural: solo ayuda cuando estas clickeando o acabas de clickear/soltar.
		// Fuera de esa ventana no gasta nada y nunca inventa saltos.
		if (!inactive && cfg.clickWindowMs > 0) {
			inactive = !g.clicking &&
				msSince(g.lastClick) > cfg.clickWindowMs &&
				msSince(g.lastRelease) > cfg.clickWindowMs;
		}

		if (inactive) {
			// Si estabamos invirtiendo tu click, devolvemos el control al jugador
			if (g.wasOverriding && m_player1 && !m_player1->m_isDead) {
				this->endOverride(g.clicking);
			}
			g.overriding = false;
			g.wasOverriding = false;
			g.intentDirty = true;
			return GJBaseGameLayer::update(dt);
		}

		bool aggressive = (cfg.principal == LvAggressive);
		bool intent = g.clicking;

		// Solo evaluamos cuando toca (por tiempo) o cuando el jugador cambio de click
		bool due = g.intentDirty || static_cast<double>(msSince(g.lastEval)) >= g.intervalMs;
		if (due) {
			g.intentDirty = false;
			g.lastEval = Clock::now();
			this->evaluate(pl, intent, aggressive, dt);
		}

		// Aplicamos la decision (solo tocamos el boton si cambia algo)
		if (g.overriding != g.wasOverriding) {
			if (g.overriding) {
				if (isHolding(m_player1) == intent) this->modClick(!intent);
			} else {
				this->endOverride(intent);
			}
			g.wasOverriding = g.overriding;
		} else if (g.overriding && isHolding(m_player1) == intent) {
			this->modClick(!intent);
		}

		GJBaseGameLayer::update(dt);
	}
};

class $modify(CFPlayLayer, PlayLayer) {
	void resetLevel() {
		if (g.simulating) return;
		PlayLayer::resetLevel();
		refreshConfig();
		g.saves = 0;
		g.assisted = false;
		g.lastSave = Clock::time_point::min();
		g.corrections = 0;
		g.overriding = false;
		g.wasOverriding = false;
		g.intentDirty = true;
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

		ensureConfig();

		int mode = cfg.secondary;
		if (mode == LvOff) {
			return PlayLayer::destroyPlayer(player, obj);
		}

		bool shouldSave = false;
		bool newSaveEvent = msSince(g.lastSave) > 200; // evita contar cada frame

		if (mode == LvAggressive) {
			shouldSave = !cfg.aggressiveRequireClick || g.clicking || msSince(g.lastClick) <= 600;
		} else { // Balanceado
			shouldSave = msSince(g.lastClick) <= cfg.balancedWindow;
			if (shouldSave && newSaveEvent && g.saves >= cfg.balancedMaxSaves) {
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
		ensureConfig();
		if (g.assisted && cfg.safeMode) {
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
		auto mode = Mod::get()->getSettingValue<std::string>("principal-mode");
		std::string next = "Balanceado";
		if (mode == "Apagado") next = "Balanceado";
		else if (mode == "Balanceado") next = "Agresivo";
		else next = "Apagado";

		Mod::get()->setSettingValue<std::string>("principal-mode", next);
		refreshConfig();
		Notification::create("Click Fix Principal: " + next, NotificationIcon::Success, 1.5f)->show();
	}
};
