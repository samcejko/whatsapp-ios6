# 📦 WhatsApp IPA Extrakce - Shrnutí

> **Quick Reference** pro extrahovaná data z WhatsApp IPA

---

## 🎯 Co jsme našli

### ✅ VYSOKÁ PRIORITA - Okamžité použití

| Kategorie | Počet | Lokace | Použití |
|-----------|-------|--------|---------|
| **🔊 Audio soubory** | 45+ | `*.m4r, *.m4a, *.caf, *.aif` | Notifikace, hovory, UI feedback |
| **🌍 Lokalizace** | 40+ jazyků | `*.lproj/Localizable.strings` | Profesionální překlady (cs: 852 řádků!) |
| **🌐 Server config** | 2 providery | `domain_fronting_providers.tsv` | Amazon AWS, Google Cloud fallback |
| **📊 Core Data** | 2 modely | `DeviceAgents.momd`, `AppState.momd` | Paired devices architektura! |
| **📱 Info.plist** | 1 soubor | `Info.plist` | Permissions, URL schemes, configs |

### 🟡 STŘEDNÍ PRIORITA - Vylepšení UX

| Kategorie | Počet | Lokace | Použití |
|-----------|-------|--------|---------|
| **🖼️ Wallpapery** | 9 JPG | `*.jpg` | Chat pozadí |
| **🎨 Ikony** | 14 variant | `AppIcon-*.png` | Alternativní ikony app |
| **🎬 Animace** | 10+ JSON | `*.json, *.mp4` | Lottie animace, video |
| **🔤 Fonty** | 2 TTF | `Optimistic_*.ttf` | WhatsApp typography |

### 🟢 NÍZKÁ PRIORITA - Reference

| Kategorie | Počet | Lokace | Použití |
|-----------|-------|--------|---------|
| **🎥 Shadery** | 10+ | `*.metal, *.metallib` | Video/graphics processing |
| **📐 UI Templates** | 100+ | `bloks_*.json`, `foa_*.json` | UI framework patterns |
| **📚 NIBs** | 6+ | `*.nib`, `*.storyboardc` | Layout reference |

---

## 🚀 Quick Start - Jak extrahovat

### Metoda 1: Interaktivní skript (doporučeno)

```bash
bash tools/extract_whatsapp_assets.sh
```

Vybereš si:
1. **Quick Wins** (30 min) - Audio + lokalizace + konfigurace
2. **Core Features** (2-3h) - Všechno důležité
3. **Full Extraction** (3-4h) - Úplně všechno
4-7. Jednotlivé kategorie

### Metoda 2: Manuální kopírování

#### Quick Wins (30 minut):
```bash
# Audio
mkdir -p Resources/sounds/calls
cp real/Payload/WhatsApp.app/incoming.m4a Resources/sounds/
cp real/Payload/WhatsApp.app/WA_call_*.m4a Resources/sounds/calls/

# České lokalizace
mkdir -p Resources/cs.lproj
cp real/Payload/WhatsApp.app/cs.lproj/Localizable.strings Resources/cs.lproj/

# Server config
mkdir -p Resources/config
cp real/Payload/WhatsApp.app/domain_fronting_providers.tsv Resources/config/

# Wallpapery
mkdir -p Resources/wallpapers
cp real/Payload/WhatsApp.app/desert_sunset.jpg Resources/wallpapers/
```

---

## 💡 Top 5 Nejužitečnějších Souborů

### 1. 🥇 `cs.lproj/Localizable.strings`
**Proč:** 852 řádků profesionálních českých překladů!
```bash
cp real/Payload/WhatsApp.app/cs.lproj/Localizable.strings Resources/cs.lproj/
```
**Dopad:** Vyřeší feedback #25 (česká kvalita)

### 2. 🥈 `DeviceAgents.momd/`
**Proč:** Core Data model pro paired devices - přesně to co potřebujeme!
```bash
cp -r real/Payload/WhatsApp.app/DeviceAgents.momd analysis/models/
```
**Dopad:** Insight do implementace companion mode

### 3. 🥉 `WA_call_*.m4a` (Call sounds)
**Proč:** 7 profesionálních zvuků pro volání
```bash
cp real/Payload/WhatsApp.app/WA_call_*.m4a Resources/sounds/calls/
```
**Dopad:** Lepší user experience při hovorech

### 4. 🏅 `domain_fronting_providers.tsv`
**Proč:** Amazon & Google fallback servery
```bash
cp real/Payload/WhatsApp.app/domain_fronting_providers.tsv Resources/config/
```
**Dopad:** Lepší connection reliability

### 5. 🏅 `*.jpg` (9 wallpaperů)
**Proč:** High-quality pozadí pro chaty
```bash
cp real/Payload/WhatsApp.app/*.jpg Resources/wallpapers/
```
**Dopad:** Profesionálnější vzhled aplikace

---

## 📊 Detailní Breakdown

### 🔊 Audio Soubory (45+)

#### Notifikační tóny (12):
```
aurora.m4r, bamboo.m4r, chord.m4r, circles.m4r, 
complete.m4r, hello.m4r, input.m4r, note.m4r, 
Opening.m4r, popcorn.m4r, pulse.m4r, synth.m4r
```

#### Zprávy (5):
```
incoming.m4a           - Příchozí zpráva
incomingshort.m4a      - Krátká varianta
incoming_message_notification.m4a
send_message.aif       - Odeslání
silent.m4a             - Tichá notifikace
```

#### Hovory (11):
```
WA_call_connect.m4a              - Spojeno
WA_call_hangup.m4a               - Ukončeno
WA_call_join.m4a                 - Vstoupil
WA_call_leave.m4a                - Opustil
WA_Pre-Connect_Series_1-4.m4a    - Čekání (4 varianty)
vc-ended.m4a                     - Video call konec
end_bot_call.m4a / start_bot_call.m4a
```

#### UI zvuky (10+):
```
Bell.m4a, Boing.m4a, Glass.caf, Harp.m4a, 
Tri-tone.m4a, TimePassing.m4a, ...
```

### 🌍 Lokalizační Jazyky (40+)

#### Top 10 pro implementaci:
```
cs (Czech) ⭐ - 852 řádků
en (English) - base
de (German) - 2. největší trh
es (Spanish) - globální dosah
fr (French) - Evropa
it (Italian) - Evropa
pt (Portuguese) - Brazílie
pl (Polish) - Východní Evropa
ru (Russian) - Rusko + SNS
nl (Dutch) - Nizozemsko
```

#### Soubory v každém jazyce:
- `Localizable.strings` - hlavní UI stringy
- `InfoPlist.strings` - app metadata
- `IQLocalizable.strings` - dodatečné stringy
- `ConnectionFAQ.json` - FAQ (některé jazyky)

### 🖼️ Wallpapery (9)

```
desert_sunset.jpg      - Pouštní západ slunce
forest_mist.jpg        - Lesní mlha
mountain_dawn.jpg      - Horský úsvit
nature_landscape.jpg   - Přírodní krajina
ocean_waves.jpg        - Oceánské vlny ⭐
rolling_hills.jpg      - Kopce
scenic_portrait.jpg    - Portrét
urban_skyline.jpg      - Město
visual_interest.jpg    - Abstraktní
```

### 🎨 Alternativní Ikony (14)

```
AppIcon-01: Black       AppIcon-08: Dark mode
AppIcon-02: Blue        AppIcon-09: Light
AppIcon-03: Green ⭐    AppIcon-10: Gradient 1
AppIcon-04: Pink        AppIcon-11: Gradient 2
AppIcon-05: Purple      AppIcon-12: Minimal
AppIcon-06: Red         AppIcon-13: Retro
AppIcon-07: Yellow      AppIcon-14: Pride
```

---

## 🔧 Integrace do Projektu

### 1. Audio do WAAudioEngine

```objc
// WAAudioEngine.h
- (void)playIncomingMessageSound;
- (void)playCallConnectedSound;
- (void)playCallDisconnectedSound;

// WAAudioEngine.m
- (void)playIncomingMessageSound {
    [self playSound:@"sounds/incoming.m4a"];
}

- (void)playCallConnectedSound {
    [self playSound:@"sounds/calls/WA_call_connect.m4a"];
}
```

### 2. Lokalizace do App

```objc
// Použití v kódu:
self.title = NSLocalizedString(@"2OtJCB", @"Hledat");
[button setTitle:NSLocalizedString(@"2WneUf", @"Odeslat") 
        forState:UIControlStateNormal];
```

### 3. Wallpaper Selector

```objc
// WASettingsViewController.m
- (void)showWallpaperPicker {
    NSArray *wallpapers = @[
        @"desert_sunset",
        @"ocean_waves",
        @"mountain_dawn"
    ];
    // Show picker...
}

// WAChatViewController.m
- (void)setWallpaper:(NSString *)name {
    NSString *path = [[NSBundle mainBundle] 
        pathForResource:name 
        ofType:@"jpg" 
        inDirectory:@"wallpapers"];
    self.backgroundImageView.image = [UIImage imageWithContentsOfFile:path];
}
```

### 4. Domain Fronting Fallback

```objc
// WARelay.m
- (void)loadFallbackProviders {
    NSString *path = [[NSBundle mainBundle] 
        pathForResource:@"domain_fronting_providers" 
        ofType:@"tsv" 
        inDirectory:@"config"];
    NSString *tsv = [NSString stringWithContentsOfFile:path 
                                              encoding:NSUTF8StringEncoding 
                                                 error:nil];
    // Parse and use as fallback
}
```

---

## ✅ Checklist pro Implementaci

```markdown
### Fáze 1: Quick Wins (30 min)
- [ ] Zkopírovat audio soubory (incoming, send_message, call sounds)
- [ ] Zkopírovat české lokalizace (Localizable.strings)
- [ ] Zkopírovat server konfigurace (domain_fronting_providers.tsv)
- [ ] Zkopírovat 2-3 wallpapery

### Fáze 2: Integrace (2 hodiny)
- [ ] Integrovat audio do WAAudioEngine
- [ ] Implementovat NSLocalizedString() v UI
- [ ] Přidat wallpaper selector do Settings
- [ ] Implementovat domain fronting fallback v WARelay

### Fáze 3: Analýza (2-3 hodiny)
- [ ] Analyzovat DeviceAgents.momd strukturu
- [ ] Studovat Info.plist permissions
- [ ] Prostudovat UI template JSONy
- [ ] Pochopit entity relationships v Core Data

### Fáze 4: Polish (1-2 hodiny)
- [ ] Přidat všechny wallpapery
- [ ] Přidat alternativní ikony (easter egg?)
- [ ] Přidat fonty (Optimistic)
- [ ] Přidat animace (confetti.json)

### Fáze 5: Kompletní lokalizace (2-3 hodiny)
- [ ] Přidat top 10 jazyků
- [ ] Otestovat přepínání jazyků
- [ ] Zkontrolovat všechny UI stringy
- [ ] Přidat language selector do Settings
```

---

## 📖 Dokumentace

**Kompletní guide:**  
[`plans/whatsapp-ipa-extraction-guide.md`](whatsapp-ipa-extraction-guide.md) - 500+ řádků detailní dokumentace

**Předchozí analýza:**  
[`plans/whatsapp-ipa-analysis.md`](whatsapp-ipa-analysis.md) - Původní analýza s prioritami

**Extraction script:**  
[`tools/extract_whatsapp_assets.sh`](../tools/extract_whatsapp_assets.sh) - Interaktivní extrakční nástroj

---

## 🎯 Co to vyřeší

| Problém | Řešení | Soubory |
|---------|--------|---------|
| Chybí audio feedback | 45+ profesionálních zvuků | `*.m4r, *.m4a` |
| Špatné české překlady | 852 official stringů | `cs.lproj/` |
| Connection issues | Amazon + Google fallback | `domain_fronting_providers.tsv` |
| Nejasná paired devices struktura | Core Data modely | `DeviceAgents.momd/` |
| Amatérský vzhled | 9 wallpaperů + 14 ikon | `*.jpg, AppIcon-*.png` |
| Chybějící permissions | Official descriptions | `Info.plist` |

---

## ⚠️ Důležité

**Legální použití:**
- ✅ Lokalizace - OK pro kompatibilitu
- ✅ Konfigurace - OK pro interoperabilitu  
- ✅ Analýza modelů - OK pro reverse engineering
- 🟡 Audio - pouze pro testování
- 🟡 Wallpapery - nahradit vlastními pro production
- ❌ Přímo kopírovat proprietární kód - NE

**Doporučení:**
Použít data primárně pro **pochopení** a **kompatibilitu** s WhatsApp protokolem.

---

**Vytvořeno:** 2026-10-20  
**Pro projekt:** whatsapp-ios6  
**Status:** ✅ Připraveno k extrakci
