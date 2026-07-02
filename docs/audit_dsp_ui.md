# Audit Megalo / MegaloHN — DSP, rendu sonore et UI

Date : 2026-07-02 — base : commit `46094ff`.

Ce document fait deux choses :

1. il définit un **workflow d'analyse reproductible** (comment mesurer chaque
   module, avec quels outils) ;
2. il applique ce workflow : **audit fonction par fonction** du DSP
   (faiblesses + pistes d'amélioration du rendu sonore), puis **audit des deux
   UI** (modgui MOD et éditeur JUCE VST3/AU), avec un focus sur la manipulation
   des paramètres ADSR, identifiée comme confuse.

Chaque constat est noté ▲ (impact fort), ● (moyen), ○ (mineur / polissage).

---

## 1. Workflow d'analyse

### 1.1 Outils existants dans le dépôt

| Outil | Rôle | Compilation |
|---|---|---|
| `tools/hn_env_test.cpp` | Pilote le vrai cœur DSP : déclenche un freeze puis mesure le RMS de sortie par fenêtres de 50 ms pour différents réglages ADSR | `g++ -O2 -DMEGALO_HN_SYNTH -Isrc tools/hn_env_test.cpp src/megaloHN_dsp.cpp -o /tmp/env_test` |
| `tools/hn_diag.cpp`, `hn_test.cpp` | Diagnostic de l'analyseur multi-F0 (notes détectées, saliences) | idem, voir `docs/hn_polyphony_testing.md` |
| `tools/hn_render.cpp` + `hn_wav.cpp` | Rendu WAV hors-ligne pour écoute A/B | idem |

### 1.2 Boucle de mesure recommandée, module par module

Le principe : **un signal de test synthétique + une métrique objective + une
écoute A/B**, pour chaque module. Concrètement :

- **FreezeEngine (détection d'onset)** — injecter des salves (burst) à
  différents niveaux (−60…0 dBFS) et différents rapports transitoire/fond ;
  compter les onsets détectés vs attendus (faux positifs sur bruit de fond,
  faux négatifs en jeu legato). Métrique : précision/rappel.
- **FreezeEngine (recherche de point de bouclage)** — geler une note tenue,
  rendre 5 s de wet pur (`blend=1`), mesurer le flux spectral à la période de
  la boucle (une discontinuité au raccord ressort comme un peigne à
  `sr/loop_len` Hz). Métrique : énergie des raies de répétition.
- **GrainPlayer** — geler un sinus pur, mesurer (a) l'ondulation d'amplitude
  du wet (pompage périodique) en fonction de `grain_size`/`grain_xfade`,
  (b) le spectre pour `pitch = ±12 st` (repliement, perte d'aigus de
  l'interpolation linéaire). Métriques : profondeur de modulation (dB),
  rapport signal/alias (dB).
- **PhaseVocoder** — même signal, comparer granulaire vs PV : « phasiness »
  (écart-type de la phase instantanée par partiel), transitoires.
- **Envelope** — `hn_env_test` couvre déjà attaque/sustain ; ajouter des cas
  decay court + sustain < 1 et vérifier le temps à 90 % mesuré vs affiché.
- **Analyse H+N (multi-F0)** — `hn_diag`/`hn_test` + le protocole de
  `docs/hn_polyphony_testing.md` (accords de 2 à 6 notes, octaves) ;
  mesurer aussi le **temps d'exécution** de `hn_multif0_analyze()` sur la
  cible (voir ▲ 2.7.1 : c'est le principal risque temps-réel).
- **Chaîne complète** — rendu WAV de scénarios réels (note seule, accord,
  jeu enchaîné rapide) sur les 10 presets, écoute A/B avant/après chaque
  modification + `loudness` LUFS pour vérifier qu'un correctif ne change pas
  le niveau perçu.

### 1.3 Automatisation proposée

- Ajouter une cible `make audit` qui compile les outils de `tools/` et rejoue
  les scénarios ci-dessus en imprimant les métriques (les harnais existants
  impriment déjà des tableaux — il manque surtout un équivalent granulaire de
  `hn_env_test`, c.-à-d. un `grain_test.cpp` mesurant pompage et alias).
- Brancher cette cible dans `.github/workflows/build.yml` pour que chaque PR
  publie les métriques (régression sonore objectivée).

---

## 2. Audit DSP, fonction par fonction

### 2.1 `FreezeEngine::process` — détection d'onset (`src/freeze_engine.hpp:69`)

- ● **Pas de plancher absolu d'énergie.** La détection est un rapport
  RMS rapide (2 ms) / RMS lent (200 ms) avec hystérésis — indépendant du
  niveau, c'est bien — mais `_rms_slow` descend jusqu'à 1e-10 : dans le
  silence, le moindre souffle/clic de médiator résiduel fait exploser le
  ratio et déclenche une capture de bruit. Piste : gater sous un seuil absolu
  (~−55 dBFS) avant d'armer l'onset. Bénéfice direct : plus de pads
  « fantômes » de bruit de fond quand `threshold` est bas.
- ○ Les constantes 2 ms / 200 ms et le facteur d'hystérésis ×1,3 sont figés ;
  ils conviennent à la guitare mais pas forcément à des sources lentes
  (archet, synthé). Piste : en faire des constantes nommées, éventuellement
  déduites de `attack_skip_ms`.
- ○ `RETRIGGER_MS = 200` fixe limite le jeu rapide à ~5 captures/s ; combiné
  au `FREEZE_POST_LOOP_HOLD_MS`, c'est cohérent, mais mérite d'être documenté
  côté UI (l'utilisateur croit à un raté de détection).

### 2.2 `FreezeEngine::_search_and_finalise` — point de bouclage (`src/freeze_engine.hpp:147`)

- ▲ **Le score du raccord favorise les zones silencieuses.** Le score est
  `Σ (head[k]−tail[k])²` non normalisé par l'énergie : une fenêtre quasi
  silencieuse obtient mécaniquement le meilleur score même si le raccord y
  est mauvais *relativement*. Piste : normaliser par
  `Σ head² + Σ tail²` (corrélation normalisée) — raccords nettement plus
  propres sur les notes qui decayent.
- ▲ **Fenêtre de comparaison trop courte pour les graves.** `cmp = min(128,
  len/4)` ≈ 2,7 ms @48 kHz, soit moins d'un quart de période du mi grave
  (82 Hz ≈ 12 ms). Sous ~350 Hz le choix du raccord est quasi aléatoire →
  battements/roulis audibles à chaque tour de boucle. Piste : estimer la
  période (autocorrélation grossière, on a déjà 120 ms de marge
  `SEARCH_EXTRA_MS`) et comparer sur ≥ 1 période, ou au minimum porter `cmp`
  à ~15 ms.
- ● **Le fondu de capture est gravé dans la boucle.** Le demi-Hann de tête
  (`capture_fade_ms`) est appliqué destructivement dans `_play[]` : ce creux
  d'énergie est relu à chaque passage par les grains → une « respiration »
  périodique de la boucle. Piste : ne faire ce fondu qu'à la première lecture
  (côté lecture), ou le compenser en gain.
- ● **Pic CPU dans le thread audio.** La recherche (≈ milliers de positions ×
  128 MAC) s'exécute inline sur *un* sample du bloc. C'est ~0,1–0,5 ms d'un
  coup — acceptable sur desktop, sensible sur MOD Dwarf à petit buffer.
  Piste : étaler la recherche sur plusieurs blocs (state machine), ou la
  déplacer dans le différé comme l'analyse HN (mais voir 2.7.1).
- ○ `FREEZE_MAX_SR = 96000` : à 192 kHz (hôtes desktop), la capture max passe
  silencieusement de 500 ms à 250 ms. À défaut de dimensionner à l'init,
  documenter/clamper explicitement.

### 2.3 `GrainPlayer` (`src/granular_looper.hpp`)

- ▲ **Somme des enveloppes non constante → pompage.** Les 4 voix sont
  décalées de `grain/4`, mais l'enveloppe trapézoïdale n'a une somme
  constante que si `xfade = grain/4` exactement. `grain_xfade_ms` étant
  indépendant (5–100 ms) de `grain_size_ms` (5–200 ms), la somme ondule à la
  fréquence `sr/grain_len` → modulation d'amplitude audible (le « pompage »
  typique du pad). Pistes : (a) crossfade équi-puissance (cos²) avec
  normalisation par la somme réelle des fenêtres, ou (b) contraindre
  `xfade ≤ grain/4` et normaliser par `Σ amp` calculée analytiquement.
  C'est probablement le gain de rendu le plus immédiat du moteur granulaire.
- ● **Interpolation linéaire seulement.** À `speed ≠ 1` (voix ±12 st), la
  lecture linéaire rabote les aigus et laisse replier le spectre au-dessus de
  `sr/2·speed` en transposition montante. Piste : interpolation Hermite
  4 points (coût marginal, ~2× la version linéaire) ; gain net de brillance
  et de propreté sur `pitch2 = +12`.
- ● **Positions de grain purement uniformes.** Deux voix peuvent respawner
  presque au même endroit → filtrage en peigne fugitif (flanging aléatoire).
  Piste : tirage stratifié (imposer une distance minimale entre positions,
  ou séquence de Halton), et éviter la zone du fondu de tête (cf. 2.2).
- ○ Le respawn n'est pas synchronisé au raccord de boucle — c'est justement
  la force du granulaire (pas de répétition) ; à garder.

### 2.4 `Envelope` (`src/envelope.hpp`)

- ● **Sémantique non standard, source de la confusion UX.** Il n'y a pas de
  note-off : le *sustain* dure indéfiniment et le *release* n'est déclenché
  que par l'onset **suivant** (le pad sortant s'éteint sous le nouveau).
  Conséquences perçues : « D ne fait rien » (avec `sustain=1.0` par défaut,
  le decay est court-circuité) et « R ne fait rien » (il ne s'entend qu'au
  prochain déclenchement). Le DSP est cohérent ; c'est le *modèle mental*
  qu'il faut réparer côté UI (voir §4), et le préréglage par défaut
  (`sustain=0.8` rendrait le decay audible immédiatement).
- ○ Attaque/décroissance exponentielles à cible fixe : correct. Le seuil de
  fin d'attaque à 0,999 ajoute ~2,2 τ de latence sur le passage en Decay pour
  les attaques longues ; sans conséquence audible.
- ○ `envelope.set()` recalcule 3 `exp()` à chaque bloc même sans changement —
  à mettre en cache comme le filtre (micro-optimisation).

### 2.5 Mixage, dry-fill et écrêtage (`src/megalo_dsp.cpp:376-431`, `src/megaloHN_dsp.cpp:534-592`)

- ▲ **`soft_clip` permanent sur la sortie, y compris le dry.** `x/(1+|x|)`
  n'est pas un limiteur : à −6 dBFS il compresse déjà de ~3,5 dB et génère de
  la distorsion d'ordre impair. À `blend=0`, le plugin ne repasse jamais le
  signal dry tel quel. Piste : écrêtage à genou (linéaire jusqu'à ~−6 dBFS,
  `tanh` au-delà), ou normaliser la somme des voix (v0+v1+v2+dry-fill peut
  atteindre ~4×) pour que le clip ne serve que de filet. Gain : transparence
  du dry, aigus du pad moins « voilés » à fort niveau.
- ● **Loi de mixage incohérente entre les deux cœurs.** MegaloHN a reçu le
  blend équi-puissance (sin/cos) ; le Megalo granulaire garde le blend
  linéaire (creux de −6 dB au centre) pour compatibilité bit-exacte. Piste :
  porter la loi équi-puissance au granulaire (éventuellement derrière un
  define) pour homogénéiser presets et sensations entre les deux plugins.
- ● **MegaloHN : le release du pad précédent est masqué.** `wet_g =
  wet_g0·(1−xfade)` coupe le wet dès l'onset (choix assumé et commenté pour
  l'anti-clic HN), là où Megalo préserve la queue de release. Piste HN :
  n'appliquer le mute qu'aux voix *additives* (celles qui vont être
  remplacées) et laisser la queue du filtre/release s'éteindre par
  l'enveloppe — rendrait l'enchaînement d'accords moins « pompé ».
- ○ Megalo : le dry-fill additif est bien conçu (couvre exactement le déficit
  de wet) ; RAS à part sa dépendance au soft-clip ci-dessus.

### 2.6 `Biquad` + LFO/détune (`src/biquad.hpp`, `megalo_dsp.cpp:267-346`)

- ● **Pas de lissage du filtre.** Les coefficients ne sont recalculés qu'au
  changement de valeur, sans interpolation : automation/mouvement du cutoff à
  Q élevé → zipper et clicks. Piste : SVF topology-preserving (Zavalishin) —
  stable en modulation par bloc, coût équivalent — ou interpolation des
  coefficients sur le bloc.
- ● **BP à gain de crête = Q.** La forme RBJ utilisée (`b0 = sinω/2·norm`) a
  un gain de crête proportionnel à Q : à Q=10, +20 dB → engage le soft-clip.
  Piste : variante « constant peak gain » (`b0 = α/norm`) pour que le knob Q
  change la largeur, pas le niveau.
- ○ `std::sin` double précision par sample pour le LFO : un oscillateur
  récursif (rotation complexe) ferait la même chose pour ~rien. Sur le chemin
  PV et sur le chemin HN, le LFO n'est échantillonné **qu'en début de bloc**
  → chorus « en escalier » à gros buffers (≥1024) et rates élevés ; à lisser
  par rampe intra-bloc si des hôtes desktop gros-buffer sont visés.
- ○ L'asymétrie du détune linéarisé (`1+(ratio−1)·lfo`) est < 1 cent à
  50 cents : négligeable, OK.

### 2.7 MegaloHN — analyse et resynthèse

#### 2.7.1 `hn_multif0_analyze` appelé dans le thread audio (`src/megaloHN_dsp.cpp:345`)

- ▲ **Violation temps-réel assumée mais dangereuse.** FFT 16384 + ~276
  candidats × combs + NNLS (40 itérations, boucle interne O((notes×partiels)²)
  ≈ 6 M op) s'exécutent au début du bloc qui suit LoopReady. Budget d'un bloc
  de 128 samples @48 kHz : 2,7 ms — l'analyse peut le dépasser sur Dwarf/Pi,
  d'où un dropout à *chaque capture*. Piste prioritaire : worker thread —
  poster le loop au worker (FIFO lock-free), continuer en fallback granulaire
  (déjà en place !) tant que `hn_state.valid == false`, et échanger le
  `MultiHNState` par pointeur atomique quand l'analyse aboutit. Le mécanisme
  `hn_trigger_pending` s'y prête déjà : l'architecture actuelle (dry tenu à
  fond jusqu'à ce que le nouvel accord existe) tolère naturellement une
  latence d'analyse de 1 à 3 blocs.

#### 2.7.2 Qualité de la resynthèse additive (`src/additive_synth.hpp`, `hn_multif0.hpp`)

- ▲ **Partiels forcés à `k·f0` exact : le pad sonne « orgue ».** Les cordes
  réelles sont inharmoniques (partiels étirés `f_k ≈ k·f0·√(1+B·k²)`) ; les
  amplitudes sont d'ailleurs déjà mesurées sur le pic réel (`peak_mag` avec
  snap ±1 bin) mais la *fréquence* resynthétisée reste `k·f0`. Piste : stocker
  aussi la fréquence raffinée de chaque partiel (l'interpolation parabolique
  est déjà écrite pour le raffinement F0) et l'utiliser dans
  `AdditiveSynth::render`. Coût nul en RT (une table de plus), gain de
  naturel important — c'est LA piste rendu sonore du moteur HN.
- ● **Phases mesurées, jamais utilisées.** `harm_phase[]` est extrait puis
  ignoré (phases à 0 au reset). Pour un pad ce n'est pas gênant, mais
  initialiser les phases mesurées rendrait la transition
  granulaire→additive/onset moins « lisse artificielle » et éviterait les
  crêtes de forme d'onde toutes alignées (facteur de crête plus élevé → clip
  plus tôt). Piste simple : initialiser `_phase[k] = harm_phase[k]`, ou à
  défaut des phases aléatoires fixes.
- ● **Bruit résiduel surestimé et statique.** `noise_rms` = puissance
  temporelle brute (attaque incluse — la suppression d'attaque ne s'applique
  qu'au *scoring*) moins puissance harmonique ; sur un pluck, le transitoire
  gonfle le bruit. Et le bruit resynthétisé (6 bandes LP, gain ×4 empirique)
  est stationnaire et mono. Pistes : mesurer le résidu sur la seconde moitié
  de la boucle ; décorréler L/R (le bruit reste centré alors que
  `hn_width` élargit les harmoniques) ; option enveloppe lente sur le bruit.
- ○ `set_timbre` non lissé : les gains par partiel sautent d'un bloc à
  l'autre en tournant Bright/Damp (zipper doux). Une rampe par bloc dans
  `render` suffirait.
- ○ Chaque `AdditiveSynth` embarque `HNState` complet + tables → 6 notes ×
  4 bancs (v0/vd/v1/v2) ; RAM ok, mais `hn_vd` coûte un banc entier
  d'oscillateurs pour un chorus — une modulation de `pitch_ratio` par voix
  avec deux jeux de phases serait moitié moins chère.

#### 2.7.3 `hn_nnls.hpp`

- ○ La boucle EM recore chaque atome contre *tous* les atomes à chaque
  itération sans précalcul des groupes de coïncidence (le commentaire les
  annonce, le code ne les matérialise pas) : précalculer les groupes une fois
  diviserait le coût par ~40. Utile surtout si l'analyse part en worker
  thread (2.7.1) — sinon invisible.

### 2.8 `PhaseVocoder` (`src/phase_vocoder.hpp`)

- ● **Pas de verrouillage de phase → son « métallique/phasé ».** Le remap de
  bins accumule les phases indépendamment par bin ; les partiels perdent leur
  cohérence verticale. Piste : identity phase locking (Laroche & Dolson) —
  détecter les pics, propager la phase des pics à leurs bins voisins. Amélioration
  classique et bien documentée du rendu.
- ○ Collision de bins : `_syn_freq[dst]` est écrasé par le dernier `k` alors
  que `_syn_mag[dst]` s'accumule ; préférer garder la fréquence du
  contributeur le plus fort.
- ○ La latence N/2 (~21 ms) n'est pas déclarée à l'hôte — sans conséquence
  ici (le wet est un pad gelé, pas du matériau aligné), à documenter.

### 2.9 Wrappers

- ● **JUCE : allocation dans `processBlock`.** `monoScratch.setSize(...)` au
  premier bloc alloue sur le thread audio. Déplacer dans `prepareToPlay`
  (`setSize(1, maxBlockSize)`).
- ○ LV2 : RAS — mapping de ports propre, chemin stéréo optionnel bien géré.
- ○ JUCE : les presets écrivent via `setValueNotifyingHost` sans
  `beginChangeGesture` — toléré par la plupart des hôtes, propre à corriger.

---

## 3. Audit UI — constats communs

Les deux UI partagent le même design (panneau 720×380, fenêtre orange
« scope », poignées temporelles en haut, ADSR en bas, rangée de knobs).
Les problèmes de fond sont donc communs, et l'ADSR concentre l'essentiel.

### 3.1 ADSR : pourquoi c'est confus (diagnostic précis)

Cinq causes se cumulent — les trois premières sont géométriques, les deux
dernières sont sémantiques :

1. ▲ **Axe de drag orthogonal à l'effet visuel.** A, D et R se règlent en
   glissant **verticalement** (`data-axis="y-up"` dans la modgui ;
   `setFromY` dans `EnvHandle`), mais ce sont des **durées** : sur la courbe,
   le point se déplace… horizontalement. L'utilisateur tire vers le haut et
   voit le point partir à droite.
2. ▲ **Renormalisation croisée.** A/D/R se partagent l'axe des temps
   proportionnellement (`x = durée/(A+D+R)`) : bouger *Attack* déplace les
   points de *Decay* et *Release* à l'écran sans que leurs valeurs changent.
   Toucher un paramètre fait bouger les trois points — feedback non local,
   très déroutant.
3. ▲ **Zones de saisie déconnectées des points.** Chaque poignée occupe une
   colonne fixe d'un quart de largeur, mais le point dessiné suit la courbe
   et peut se trouver dans la colonne d'un *autre* paramètre (ex. A=0, D=0 :
   les points A et D sont collés à gauche ; le point R peut être au milieu,
   dans la colonne de S — cliquer dessus règle Sustain). De plus, la ligne de
   seuil (`ThresholdLine`, ajoutée au-dessus, hit-test ±6 px sur **toute la
   largeur** ; défaut 0,15 → elle traverse précisément la zone ADSR) vole les
   clics des poignées.
4. ● **Mapping linéaire des durées.** 0–5000 ms (A, D) et 0–10 s (R) sur
   ~85 px de course verticale ⇒ ~60 ms par pixel : impossible de régler une
   attaque de 20 ms. La plage musicale utile (< 300 ms) occupe 6 % de la
   course.
5. ● **Sémantique non standard invisible** (cf. 2.4) : sustain infini,
   release déclenché par l'onset suivant, decay inaudible avec le défaut
   `S=1.0`. Rien dans l'UI ne l'explique.

### 3.2 ADSR : refonte recommandée

Recommandation principale (commune modgui + JUCE), par ordre de priorité :

1. **Points directement draggables en 2D sur la courbe.**
   - Point A : drag **horizontal** = temps d'attaque.
   - Point D : drag horizontal = temps de decay, drag **vertical** = sustain
     (le niveau d'arrivée du decay *est* le sustain — un seul point pour deux
     paramètres liés, c'est le pattern standard des éditeurs d'enveloppe).
   - Point R : drag horizontal = temps de release.
   - Saisie : prendre le point **le plus proche du curseur** (rayon ~14 px),
     pas des colonnes fixes. Supprimer les poignées-colonnes.
2. **Axe des temps stable.** Échelle X fixe et log (10 ms → 10 s) plutôt que
   proportionnelle : bouger A ne déplace plus D et R. Le segment de sustain
   garde sa largeur fixe (déjà le cas, 18 %).
3. **Mapping log des durées** partout : JUCE
   `NormalisableRange::setSkewForCentre(250 ms)` sur `env_attack`/`env_decay`
   (centre 1 s pour `env_release`) ; modgui `raw = min·(max/min)^pct` (avec un
   epsilon pour min=0, p. ex. plancher 1 ms).
4. **Neutraliser les conflits de saisie** : hit-test de la `ThresholdLine`
   restreint au **demi-panneau supérieur** + à un rayon autour de sa pastille
   de gauche ; z-order sous les points ADSR.
5. **Expliquer la sémantique dans l'UI** : renommer les libellés courts en
   `FADE IN / SHAPE / LEVEL / FADE OUT` (ou garder A/D/S/R avec tooltips
   « Release : appliqué au pad sortant lors de la prochaine capture ») ;
   passer le défaut `env_sustain` à ~0,8 pour que Decay soit audible
   immédiatement.
6. **Confort JUCE** : double-clic = retour au défaut, molette, Shift = drag
   fin, affichage de la valeur pendant le drag (le modgui a déjà le tooltip ;
   l'éditeur JUCE n'affiche rien pendant la manipulation des knobs).

Alternative minimale si la 2D est jugée trop coûteuse : 4 rotatifs classiques
A/D/S/R sous la fenêtre + courbe **en affichage seul** — moins élégant mais
supprime 1–3 d'un coup.

### 3.3 Autres constats — modgui (`megalo.lv2/modgui/`)

- ● **Axe temporel mensonger.** La règle 0–500 ms au-dessus de la fenêtre
  suggère que les poignées SAMPLE/SKIP/SIZE/XFADE s'y alignent, mais chaque
  poignée mappe sa colonne sur son propre min–max (SAMPLE 150 ms s'affiche à
  22 % au lieu de 30 % ; XFADE 5–100 ms occupe une pleine colonne). Piste :
  soit positionner les poignées sur l'axe réel (SAMPLE et SKIP partagent
  0–500), soit supprimer les étiquettes numériques de la règle.
- ● **`dry_level` n'a aucun contrôle** (nommé « Dry Level (temp) » dans
  `modgui.ttl`, absent du HTML) : réglable seulement via la vue générique.
  À exposer (petit knob GLOBAL) ou à retirer du `.ttl` s'il est destiné à
  disparaître.
- ○ La forme d'onde du « scope » est décorative et statique — acceptable,
  mais un rafraîchissement du dessin à partir de `trigger_pulse` (déjà
  branché pour le flash) pourrait au moins animer la capture (feedback
  « j'ai gelé quelque chose »).
- ○ `formats` dans `script-megalo.js` ne couvre pas `dry_level` ni les
  `hn_*` (fallback `.toFixed(2)` correct mais sans unité).
- ○ Les deux bundles modgui (megalo / megaloHN) sont des copies divergentes
  du même code (le diff montre 5 fichiers différents) : factoriser le JS/CSS
  commun ou scripter la synchronisation, sinon chaque correctif devra être
  appliqué deux fois.

### 3.4 Autres constats — éditeur JUCE (`juce/PluginEditor.*`)

- ● **Grain Size / Grain Crossfade absents de l'éditeur** alors que le moteur
  par défaut (granulaire) les utilise pleinement — ils n'existent qu'en vue
  générique hôte. Le bandeau « WINDOW | GRAIN » est même toujours dessiné
  alors que les poignées GRAIN n'existent pas (`WindowPanel::paint` trace le
  libellé, `topHandles` n'a que SAMPLE/SKIP). Piste : réintroduire les deux
  poignées (comme la modgui), éventuellement masquées quand
  `pitch_mode = PV`… mais elles pilotent aussi la base/detune, donc les
  garder visibles est plus juste.
- ● **Aucune valeur affichée sur les knobs** (`NoTextBox`, pas de tooltip ni
  de popup de drag) : impossible de connaître le cutoff en Hz ou le pitch en
  demi-tons sans la vue générique. Piste : bulle de valeur pendant le drag
  (réutiliser `fmtValue`, qui ne gère aujourd'hui que « S » et les ms — à
  étendre en Hz/st/cents), et libellés au format modgui.
- ● **`dry_level` sans UI** (comme la modgui) et **pas d'indication du
  moteur HN** : l'éditeur HN ajoute la rangée TIMBRE mais rien n'indique si
  l'analyse a trouvé des notes (fallback granulaire silencieusement actif).
  Un simple témoin « HN: n notes / granular fallback » (le DSP expose déjà
  tout en interne ; il manque juste un getter comme `megalo_dsp_trigger`)
  transformerait le débogage utilisateur.
- ○ Fenêtre non redimensionnable (720×380 fixe) : tout est vectoriel, donc
  `setResizable(true, true)` + `setResizeLimits` avec ratio fixe est
  quasi gratuit.
- ○ Cibles minuscules : knobs 34 px, LED 10×10 px (la LED d'activation de
  voix est facile à rater) ; passer les LED à ≥ 16 px de zone cliquable.
- ○ Doublons de légende (« PITCH », « LEVEL », « BLEND » ×2) : acceptable car
  groupés, mais les tooltips (nom complet du paramètre) manquent.
- ○ `fmtValue` traite tout sauf « S » comme des ms : le jour où un readout
  est ajouté aux knobs, il affichera « 3000 ms » pour un cutoff. À étendre.

### 3.5 Presets

- ○ Les 10 presets `.ttl` sont dupliqués dans `juce/megalo_presets.h`
  (générés par `tools/gen_presets.py` — bien) ; ajouter la génération à la CI
  éviterait la dérive.
- ○ Aucun preset ne touche `dry_level`/`pitch_mode` : cohérent tant qu'ils
  restent cachés, à revisiter avec §3.3/3.4.

---

## 4. Priorisation suggérée

| # | Item | Réf. | Effort | Impact |
|---|---|---|---|---|
| 1 | Refonte interaction ADSR (drag 2D sur la courbe, axe temps stable, mapping log, hit-test threshold) | §3.1–3.2 | Moyen | ▲▲ UX (le point de douleur signalé) |
| 2 | Normalisation du score de raccord + fenêtre ≥ 1 période | §2.2 | Faible | ▲ rendu (boucles graves propres) |
| 3 | Somme d'enveloppes constante dans `GrainPlayer` | §2.3 | Faible | ▲ rendu (pompage) |
| 4 | Analyse HN dans un worker thread | §2.7.1 | Moyen | ▲ fiabilité RT (dropouts à la capture) |
| 5 | Partiels à fréquence mesurée (inharmonicité) | §2.7.2 | Faible | ▲ rendu HN (moins « orgue ») |
| 6 | Écrêtage à genou au lieu du soft-clip permanent | §2.5 | Faible | ● transparence |
| 7 | Interpolation Hermite dans `GrainPlayer` | §2.3 | Faible | ● brillance des voix pitchées |
| 8 | Grain Size/XFade + valeurs + `dry_level` dans l'éditeur JUCE | §3.4 | Faible | ● UX desktop |
| 9 | SVF/lissage filtre + BP gain constant | §2.6 | Moyen | ● automation propre |
| 10 | Gate absolu d'onset | §2.1 | Faible | ● faux déclenchements |
| 11 | Phase locking PV ; phases mesurées HN ; bruit HN | §2.8, 2.7.2 | Moyen | ● rendu |
| 12 | `make audit` + métriques en CI | §1.3 | Moyen | ● non-régression |

Les items 2, 3, 5, 6, 7 et 10 sont indépendants et chacun testable avec le
workflow du §1.2 ; c'est l'ordre d'attaque recommandé côté son. Côté UI,
l'item 1 se fait en deux passes (JUCE d'abord — plus simple à itérer — puis
portage modgui pour garder la parité visuelle).

---

## 5. Mise en œuvre (2026-07-02, même branche)

Implémentés dans cette passe :

- **#1 ADSR** — JUCE : nouvel `EnvelopeEditor` (points A/D/R déplacés
  horizontalement sur la courbe, S verticalement, saisie au point le plus
  proche, axe temps fixe par segments avec skew log
  `setSkewForCentre(250/1000 ms)`, hit-test de la `ThresholdLine` restreint).
  modgui (2 bundles) : drag délégué au conteneur avec choix du point le plus
  proche, A/D/R horizontaux, échelle log (`data-scale="log"`), courbe à axe
  fixe. Défaut `env_sustain` 1.0 → 0.8 partout (Decay audible d'emblée).
  Vérifié : 0–300 ms occupe désormais 67 % de la course (6 % avant).
- **#2 Raccord de boucle** — score normalisé par l'énergie + fenêtre 10 ms
  (`SEAM_CMP_MS`), pas de recherche adapté pour garder le coût borné.
- **#3 Pompage granulaire** — normalisation par la somme réelle des
  enveloppes (×0,75 pour conserver le niveau). Mesuré : le niveau du pad ne
  dépend plus des réglages grain/xfade (avant : ±3 dB selon réglages).
  Note : l'ondulation stochastique résiduelle mesurée sur sinus pur vient des
  annulations de phase entre grains (voir « pistes restantes »).
- **#5 Inharmonicité HN** — `harm_freq[]` mesuré par partiel (parabolique,
  snap ±3 bins, clamp ±3 %) et utilisé par `AdditiveSynth`. Vérifié sur corde
  synthétique raide (B = 4e-4) : partiels suivis à ±1,3 cents (l'idéal k·f0
  était faux de 15 cents au partiel 7). Détection multi-F0 inchangée (18/20).
- **#6 Écrêtage** — clip à genou (linéaire < −3 dBFS, tanh au-delà, borné ±1)
  dans les deux cœurs ; le dry repasse propre à niveau normal.
- **#7 Interpolation** — Catmull-Rom 4 points dans `GrainPlayer::_read`.
- **#8 UI JUCE** — poignées SIZE/XFADE restaurées, knob DRY dans GLOBAL,
  bulle de valeur au drag + suffixe d'unité + double-clic reset sur tous les
  knobs.
- **#10 Gate d'onset** — plancher absolu ~−55 dBFS (`ONSET_ABS_GATE`).

Validation : LV2 megalo + megaloHN compilent ; `hn_env_test` (ADSR),
`hn_test` (18/20 inchangé), test d'inharmonicité et test de pompage ajoutés
en scratch ; scripts modgui vérifiés par `node --check`. La compilation JUCE
n'est pas possible dans cet environnement (accès GitHub restreint au dépôt →
FetchContent JUCE bloqué) ; elle est couverte par la CI GitHub Actions
(macOS + Windows) au push.

Pistes restantes (inchangées) : #4 worker thread pour l'analyse HN, #9
SVF/lissage filtre + BP gain constant, #11 phase locking PV / phases mesurées
HN / bruit HN stéréo-enveloppé, #12 cible `make audit` en CI.

### Deuxième passe (retours utilisateur : clacs dry→wet, grains pas lisses)

- **Clacs éliminés (mesuré)** — deux discontinuités distinctes :
  MegaloHN sautait `xfade` 0→1 en un échantillon à chaque onset (marche
  simultanée sur dry_g ET wet_g ; mesuré ×70 la pente d'entrée) → rampe de
  15 ms (`XFADE_UP_MS`). Megalo forçait `comp_level = 1` à LoopReady et
  coupait le release en cours (`envelope.reset()`) → release plafonné
  (`release_capped`, 0,6 × durée de capture : quasi éteint avant le swap de
  boucle) et suppression du saut forcé. Après correction, aucun pas de
  sortie au-dessus de la pente d'entrée sur le cœur granulaire.
- **Vraie cause du pompage : le raccord de boucle lui-même.** Le crossfade
  gravé en queue mélangeait la queue avec la TÊTE de la boucle — décalés de
  `len mod T` pour un contenu périodique — gravant une encoche d'annulation
  de ~20 ms (mesurée à 40 % d'amplitude) traversée par chaque grain, et
  faussant l'analyse HN (f0 à −4 %, resynthèse 3× trop forte). Correctifs :
  (1) crossfade contre le contenu ADJACENT de l'enregistrement (pré-tête ou
  post-queue), (2) **rognage de la boucle à un nombre entier de périodes**
  (estimation par autocorrélation normalisée + hill-climb + raffinement
  parabolique, `FreezeEngine::period()`), (3) suppression du fondu de tête
  gravé (inutile en lecture granulaire aléatoire), (4) respawn de grains
  **aligné en phase** par corrélation directe avec la voix de référence
  (`GrainPlayer::_aligned_pos`). Résultat mesuré : pompage sur sinus gelé
  24–30 dB → **1,3–4,3 dB**, f0 HN exact, resynthèse recalibrée (0,205 vs
  boucle 0,216).
- **modgui** : rangée de blocs recalée sur les marges de la fenêtre (knobs
  42→40 px, gap HN 10→8 px — le bloc GLOBAL débordait de 22–30 px et était
  rogné par le bord du panneau) ; marge morte de 10 px sous les poignées
  hautes (plus de saisie accidentelle de SAMPLE/SKIP en réglant l'ADSR,
  parité JUCE) ; potentiomètres restructurés avec un rotor transparent
  (`.megalo-knob-rotor` porte le mod-role) pour que seul le curseur orange
  tourne, l'ombrage du cadran restant fixe.
