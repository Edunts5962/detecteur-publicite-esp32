# Détecteur de pub v0.9.72-35P — portage sur la carte ES3C35P (3,5")

Repart de la v0.9.43 (carte ES3C28P, 2,8"). Le moteur (empreintes, bibliothèque,
détecteur, coupure, télécommande Wi-Fi, portail, base de pubs) est inchangé.

## v0.9.72 — portail : Détecteur et Télé dans le bandeau du haut

- L'interrupteur **Détecteur** (marche / arrêté) est en haut à droite du bandeau de l'état ; la ligne **Télé**
  (pastille, IP, connectée, volume) juste dessous, un clic ouvre l'onglet Télé. La carte du bas disparaît.
  Arrêté : bandeau gris « EN PAUSE · Détecteur arrêté ». Aucune fonction changée, écran de la carte inchangé.

## v0.9.71 — mémoire interne : mot-clé vocal supprimé, tableaux en PSRAM

Variables fixes en mémoire interne : **103 876 → 80 604 octets (−22,7 Ko)**.
- **Mot-clé vocal supprimé** (idée abandonnée) : `motcle.cpp/.h`, appel dans le moteur, carte « Mot-clé vocal »
  des Réglages du portail, `/api/motcle` (que la page Réglages interrogeait toutes les 0,7 s), coupure forcée par la
  voix et mention « mot-clé » des dernières coupures. −13 Ko interne, −14 Ko PSRAM, un calcul de moins toutes les
  20 ms (cœur 0). Le dossier `/pub/voix/` reste sur la carte SD (inutile). gestion_pubs.py inchangé.
- **En PSRAM** : tableaux de travail de la sauvegarde (5,5 Ko, rendus à la fin ; erreur de la v0.9.70), spectre de
  l'écran (4 Ko), tampons de réception de la télé (4 Ko). Calcul des empreintes laissé en mémoire interne (vitesse).
- **/api/etat et l'état de la captation** fabriqués dans des tampons en PSRAM (`TamponJ`) au lieu de nombreux petits
  `String` en mémoire interne (moins de dispersion : le plus grand bloc baisse moins).
- **Mesure des piles** : toutes les 5 min, `[PUB] piles jamais utilisées : audio … tele … reseau … octets` (pour
  décider plus tard quelles piles réduire). Aucune pile changée.
- Vérifié : sauvegarde .pub toujours identique octet pour octet (banc PC) ; simulation de coupure identique ;
  portail sans erreur JavaScript.
- Compilé : 1 587 994 octets (50 %).

## v0.9.70 — sauvegarde .pub rapide, sans bloquer le portail

Essai v0.9.69 : « Sauvegarder maintenant » → « Sauvegarde impossible » et mémoire interne ROUGE (15 Ko, bloc 7 Ko).
Cause : l'export relisait toute la bibliothèque une fois PAR PUB (162 fois) et ouvrait le fichier ~700 fois ; au-delà
de 15 s le portail abandonnait (l'écriture continuait) ; pendant ce temps le serveur web était bloqué et les requêtes
du navigateur s'empilaient en mémoire interne ; la détection était en pause.
- `base::exporter()` : un comptage, puis les pubs par paquets dans un tampon de 256 Ko (PSRAM, rendu à la fin ;
  128/64/32/16 Ko si la mémoire manque) : une lecture de la bibliothèque et une écriture SD par paquet.
  Banc PC (149 pubs, 399 030 entrées, 3,2 Mo) : **fichier identique octet pour octet** à l'ancien, 14 ouvertures
  du fichier au lieu de 1 007 ; identique aussi avec un tampon de 16, 64 ou 128 Ko. Taille du fichier vérifiée à la fin.
  Retour : nombre de pubs, -1 écriture SD, -2 mémoire, -3 fichier incomplet.
- `/api/sauvegarder` ne bloque plus : réponse « EN_COURS » tout de suite ; la page demande `op=etat` chaque seconde
  et affiche « Sauvegarde en cours… N s », puis « N pubs sauvegardées dans … (N s) » ou la vraie raison de l'échec.
  Le résultat est aussi écrit dans la console. Copie avant « Tout effacer » : attente portée à 60 s.
- Format .pub inchangé (anciennes sauvegardes et gestion_pubs.py compatibles).
- Compilé : 1 596 610 octets (50 %).

## v0.9.69 — mémoire interne sur l'accueil, fausse « pub trop courte »

- **Mémoire interne** : 5e ligne de « En ce moment » (🧠 Mémoire · libres · plus grand bloc · mini depuis le
  démarrage). Vert = plus grand bloc ≥ 16 Ko (la télé peut se reconnecter) ; orange = 8 à 16 Ko ; rouge = moins de
  8 Ko ou moins de 15 Ko libres, avec un bandeau rouge « ⚠ Attention : surcharge de la mémoire interne » sous l'état.
  `/api/etat` → `ec.ml`, `ec.mb`, `ec.mm` (Ko). Console à chaque changement de couleur (tout de suite si ça empire,
  après 10 s stables si ça s'améliore) : `[PUB] Mémoire interne : ORANGE (chargée) · N Ko libres, plus grand bloc M Ko`.
- **Bug corrigé** : « Pub trop courte · 268435,3 s » en APPRENDRE / DÉCOUPER. Le silence commençait juste avant le
  début du morceau ouvert à la fin d'une pub connue (marge) : durée négative calculée sans signe. `au_courte()`
  calcule avec le signe ; moins de 1 s ou négatif = ignoré sans être compté. Aucune pub n'était perdue.
- Compilé : 1 594 218 octets (50 %).

## v0.9.68 — accueil du portail réorganisé

- Actions en haut : état → **TESTER** (ou « SON BAISSÉ · Remettre le son ») → compteurs → préréglages →
  « En ce moment » (phrases raccourcies) → spectre → dernières coupures → Détecteur marche/arrêt et Télé dans une
  seule carte → date de la carte en bas. Aucune fonction changée, écran de la carte inchangé.
- Contient tout la v0.9.67. Désormais un nouveau numéro à chaque livraison.
- Compilé : 1 592 494 octets (50 %).

## v0.9.67 — corrections mineures + page Capter du portail

- **Optique : (gauche + droite) / 2** au lieu de la seule voie gauche (flux continu et TEST FO). Les pubs déjà
  apprises restent reconnues (les deux voies d'une pub sont presque identiques).
- **Coupure fibre : âge de la preuve compté une seule fois.** `pos_s` est déjà ramené à « maintenant » par le
  détecteur ; `fibre_reconnue()` ne retire plus `age_match_s` (la fin prévue du spot était trop tôt de cet âge, un
  palier pouvait partir un peu tôt). Simulation PC : premier palier 6,0 s après la fin du spot, avec ou sans âge.
- **Sandwich** : marges de 1 s (au lieu de 0,3 s) de chaque côté du passage ; pendant une coupure, « déjà connue »
  seulement si la pub reconnue couvre au moins 60 % du passage (sinon apprise, à vérifier, raison « ressemble ») ;
  le vrai résultat est écrit dans le journal : `[PUB] Apprendre (coupure) : … apprise / déjà connue (score, %) /
  trop court / presque sans son / bibliothèque pleine`.
- **Portail, onglet Capter réorganisé** : titre « Capter les pubs » (la captation suit la source, micro ou
  optique) ; chaque bouton a son explication juste dessous ; ordre et noms : 🧠 APPRENDRE les nouvelles pubs,
  ✂ DÉCOUPER les pubs en fichiers (ancien AUTO), ⏺ TOUT ENREGISTRER (ancien CAPTER). Même fonctionnement.
- **TEST FO déplacé** dans Réglages (portail), carte « Vérifier l'entrée optique » sous « Détection ».
  L'écran de la carte n'a pas changé (TEST FO toujours dans l'onglet Capter de l'écran).
- Pas de mise à jour par le portail (partition inchangée : Huge APP).
- Compilé : 1 592 526 octets (50 %).

## v0.9.66 — 4 préréglages (portail et écran)

Un préréglage garde : source du son, coupure, volume pendant la pub, durée d'une coupure, après la pub, sensibilité,
gain du micro, réécoute (ni télé, ni Wi-Fi, ni écran, ni LED). Gardés à part en NVS (`prereg1`) : les réglages
courants ne sont pas remis à zéro. Au premier démarrage : 📡 Optique confort (optique, fenêtres, son 1, 45 s),
⚡ Optique rapide (idem, 20 s), 🎤 Micro (micro, fenêtres, son 3, 30 s), ⭐ Mon réglage (copie des réglages actuels).
Tous : retour au volume d'avant, sensibilité normale, gain automatique, réécoute 8 s.
- **Portail, accueil** : 4 gros boutons sous « En ce moment » ; un appui applique tout de suite ; le bouton actif est
  vert ; l'encadré dit « préréglage : … » ou « réglage personnalisé » (réglages ≠ toutes les cases).
- **Portail, Réglages** (en haut) : choisir la case 1 à 4, nom (19 caractères, sans " \\ < >), « Enregistrer » (les
  réglages actuels) ; « Remettre les préréglages d'origine » (la case 4 reprend les réglages actuels).
- **Écran** : onglet Réglages, nouvelle première page « Profils » (barre : Profils · Son télé · Détection · Carte ·
  Wi-Fi), 4 cases ; toucher = appliquer ; la case active est en vert. Enregistrement depuis le portail seulement.
- API : `GET /api/prereg` (`a` actif, `p` liste) ; `POST /api/prereg` op = appliquer | enregistrer | origine, `i`, `nom` ;
  `/api/etat` → `ec.pr`. `cfg::prereg_*`.
- Compilé : 1 591 558 octets (50 %).

## v0.9.65 — remontée du son en 4 paliers jusqu'au volume d'avant la pub

Essai v0.9.64 : mi-volume OK (volume télé 10, compte à rebours), mais remontée en une marche. La barre de volume
de la TCL s'affiche à chaque changement : on garde peu de changements.
- Fin d'écran probable (`cpr::DEMI_MS` = 6 s après la fin du dernier spot reconnu) : **paliers 1, 2, 3** à 0 %, 30 %
  et 62 % de la suite de l'attente, aux volumes bas + 10 %, 30 %, 65 % de l'écart jusqu'au **volume d'avant la pub**
  (`v_origine`, ou niveau haut si « niveau haut fixe ») ; puis, à la fin de l'attente, `REMONTER` remet **exactement**
  le volume d'avant. Un palier ne peut jamais atteindre ce volume (au plus volume d'avant − 1) ; un palier égal au
  volume actuel n'envoie aucun appui (pas de barre).
- Exemple (bas 1, avant 15, attente 45 s) : fin de la dernière pub → 6 s : 2 → 18 s : 5 → 30 s : 10 → 45 s : 15.
- Pub reconnue pendant la remontée → retour immédiat au niveau bas (une seule barre).
- `cpr::niveau()` : 0 normal, 1 bas, 2..4 paliers ; `cpr::palier()` ; `tv::suivre_niveau()` (% du palier).
- Écran : « LE SON REMONTE · palier 2/4 · 0:28 » ; portail : « LE SON REMONTE (palier 2/4) · volume d'avant dans … » ;
  Réglages : rappel « volume pendant la pub : au moins 1 (0 coupe aussi la sortie optique sur certaines télés) ».
- Simulation PC (`tests/cpr/t3`) : écran connue/inconnue/connue/inconnue/connue puis programme → volume 1 pendant les
  pubs connues, paliers 2 puis 5 pendant les inconnues, redescente à 1, fin : 2 → 5 → 10 → 15.
- Compilé : 1 582 978 octets (50 %).

## v0.9.64 — fin d'écran de pub : mi-volume, apprentissage « en sandwich », spectre sur le portail

À essayer ; en cas de souci, revenir à la v0.9.63 (aucun réglage changé : retour sans risque).

**Retour du son en deux temps** (entrée optique, coupure « par fenêtres ») :
- 6 s (`cpr::DEMI_MS`) après la fin du dernier spot reconnu sans nouvelle pub → le son remonte **à mi-chemin**
  entre le niveau bas et le volume d'avant (`tv::MILIEU`, seulement si le volume de la télé est connu) ;
- une pub reconnue → de nouveau tout bas ; `coupure_s` sans pub reconnue → volume normal.
- Deux pubs connues collées (reconnue en ~3 s) : pas de remontée entre elles (simulation `tests/cpr/t2`).
- « Tu as remis le son » : seuil relevé au niveau mi-chemin pendant cette phase.
- `cpr::niveau()` (0 normal, 1 bas, 2 mi-chemin), `tv::suivre_niveau()`.

**Pubs inconnues « prises en sandwich »** (entrée optique, pendant une coupure) : un passage de 5 à 60 s entre la
fin d'une pub reconnue et le début de la suivante (même coupure) est appris (`CMD_APPRENDRE_AUTO`, bits 256 | 512),
origine « auto », toujours **« à vérifier »** (nouvelle raison `DOUTE_COUPURE` = 5 : « apprise pendant une coupure,
entre deux pubs »), son gardé (archive). Pas de doublon (déjà connue → rien). Console :
`[PUB] Coupure : N s inconnues entre la pub n°A et la pub n°B : apprise (à vérifier)`.

**Spectre sur le portail** : spectrogramme défilant (24 bandes, comme l'écran de la carte) sous les compteurs,
`/api/spectre` (24 octets, calcul existant). 4 lectures/s au plus, seulement accueil affiché et page visible, en pause
pendant une captation ou un TEST FO.

Divers : tampon de `/api/etat` en PSRAM (RAM interne juste). Écran : « SON À MI-VOLUME · normal dans 0:14 ».
Compilé : 1 582 546 octets (50 %).

## v0.9.63 — coupure « fibre » sans remontée du son, encadré « En ce moment »

Contient aussi la v0.9.62 (TEST FO en mode Optique), pas encore essayée sur la carte.

**Coupure en entrée optique** (mode « par fenêtres », source optique avec du son reçu) : la sortie optique de la
télé reste à niveau fixe, la carte entend donc la pub même son baissé. Le son n'est **plus remonté pour réécouter** :
- pub confirmée (1,5 s) → son baissé au niveau réglé ;
- chaque pub reconnue pendant la coupure repousse la fin (`coupure_s` après la dernière reconnaissance) ;
- plus aucune pub reconnue pendant `coupure_s` → le son revient directement (pas de réécoute) ; plafond 12 min ;
- la fibre ne donne plus de son pendant 5 s (`cpr::FIBRE_PERDUE_MS`) → retour au fonctionnement normal
  (fenêtres + réécoute) pour ne jamais laisser le son baissé à l'aveugle ;
- micro, mode « suivre la pub », TESTER, captations, mot-clé, « Remettre le son » : inchangés.
- `cpr::ecoute_fibre` (posé par le moteur : source optique et son reçu), `cpr::coupe_fibre()`.
- Simulation PC (`tests/cpr`) : écran pub connue 30 s + inconnue 20 s + connue 30 s → son baissé d'un seul tenant,
  revenu 30 s après la dernière pub reconnue ; fibre perdue pendant une coupure → fenêtre normale puis réécoute.
- Affichage : écran « SON BAISSÉ · suit la pub · 0:28 » ; portail « SON BAISSÉ · j'écoute la fibre · remonte dans
  … sans nouvelle pub » ; Réglages : explication sous « Durée d'une coupure ».

**Portail, accueil** : encadré **« En ce moment »** en gros caractères, 4 lignes avec point de couleur, chacune ouvre
le bon onglet : Source (micro + gain + niveau / optique + état réel), Détection (marche, sensibilité, nombre de pubs),
Coupure (mode en clair, version fibre), Captation (aucune / type, durée, pubs). Compteurs en gros (pubs · à vérifier ·
détections · coupures, échec de sauvegarde en rouge). Bas de page : version seulement. `/api/etat` → `ec`.
Écran de la carte : inchangé (pas de place).

Compilé : 1 578 598 octets (50 %).

## v0.9.62 — TEST FO en mode Optique

Essai v0.9.61 : bascule micro ↔ optique OK, micro bien coupé en optique. TEST FO depuis le portail en mode
Optique : « Test optique impossible : entrée optique occupée, réessaie » (le flux continu ne rendait pas le
port I2S dans les 500 ms ; un message sur le port USB peut bloquer quand le PC ne lit pas la console).
- Le portail n'attend plus : TEST FO demande l'arrêt du flux, la tâche de test attend elle-même (3 s au plus).
- Le flux rend le port I2S **avant** d'écrire « entrée optique arrêtée » ; retour au micro : attente 1,5 s au plus.
- Compilé : 1 575 302 octets (50 %).

## v0.9.61 — VRAIE cause du « Optique: démarrage » : le réglage n'appelait pas l'optique

Essai v0.9.60 : toujours « Optique: démarrage », micro toujours actif. Contrôle des symboles du programme
compilé (`nm`) : la fonction `plat_source()` de la carte **n'existait même pas** dans le binaire. Dans
`config.cpp`, sa définition par défaut (vide, pour le PC) était écrite **dans** `namespace cfg` : elle créait
une autre fonction, `cfg::plat_source()`, que `cfg::appliquer()` appelait à la place de la vraie. Depuis la
v0.8.0, choisir « entrée optique » ne démarrait donc jamais l'optique (seul TEST FO, appelé directement, marchait).

- Définition par défaut sortie du namespace, appel explicite `::plat_source()`. Vérifié dans le binaire :
  `plat_source(int)`, `fo_surv_tache`, `fo_flux_tache` présents ; plus de `cfg::plat_source`.
- Même défaut trouvé pour `plat_appui_zone()` (retour visuel au toucher, v0.9.6, jamais actif) :
  **laissé inactif volontairement** (jamais essayé sur cet écran), noté dans le code.
- Compilé : 1 575 334 octets (50 %).

## v0.9.60 — mode Optique fiable (micro ↔ optique)

Essai du 07/11 : TEST FO bon (474 927 échantillons non nuls sur 480 000, crête 32 %), mais en mode Optique
l'écran restait sur « Optique: rien », le micro captait toujours (voix visible sur le vumètre) et aucune pub
n'était reconnue. Cause supposée : un choix « Optique » fait pendant un TEST FO (ou un démarrage raté) perdu sans
message (vraie cause trouvée en v0.9.61). Les protections ci-dessous restent utiles.

- **Surveillant** (tâche `fo_surv`, cœur 1) : tant que la source choisie est l'optique, l'entrée optique est
  démarrée, relancée après un TEST FO, et réessayée toutes les 5 s si elle ne démarre pas (raison dans la console).
- Le micro n'est coupé **que** pendant que l'optique écrit vraiment dans l'anneau ; il reprend dès qu'elle s'arrête.
- **TEST FO possible en mode Optique** : le flux est arrêté pendant les 10 s du test puis relancé tout seul.
- **État réel affiché** : écran (bandeau) « OPTIQUE » (son reçu) · « Optique: silence » · « Optique: test » ·
  « Optique: démarrage » (rouge : le micro écoute encore) ; Réglages idem ; portail : pied de page
  « source : … », `/api/etat` → `fo` (-1 micro, 0 démarrage, 1 silence, 2 son reçu, 3 test).
  Écran « silence » : « Pas de son sur l'entrée optique » au lieu de « trop loin du micro ».
- Console : `[FO] source du son : …`, `[FO] entrée optique en service …, micro coupé`,
  `[FO] entrée optique arrêtée, retour au micro`, bilan toutes les 5 s pendant 30 s puis toutes les minutes.
- Compilé : 1 573 614 octets (50 %), aucun nouvel avertissement.

## v0.9.59 — entrée optique corrigée

- **LRCK faux (cause du « Optique: rien »)** : cases I2S de 32 bits mais largeur de WS laissée à
  la valeur par défaut de la bibliothèque (16) → LRCK haut 16 tops sur 64 au lieu d'un carré
  32/32. Le module ne trouvait pas les voies. Corrigé (`ws_width = 32`) dans un réglage commun
  `fo_regler_std()` utilisé par le mode Optique et par TEST FO.
- **TEST FO** : ne garde plus que `fo_test_16k.wav` (320 Ko de PSRAM au lieu de 1,28 Mo ; il ne
  restait que ~900 Ko libres → échec « mémoire PSRAM »). La console affiche la PSRAM libre au départ
  et un bilan (échantillons, non nuls, crête).
- **Diagnostic** en mode Optique : une ligne toutes les 5 s,
  `[FO] 5 s : N échantillons (attendu 240000), M non nuls, crête …, lectures vides …`.
  - attendu atteint mais 0 non nul : horloges OK, le module n'envoie que des zéros ;
  - beaucoup moins qu'attendu / lectures vides : l'I2S de l'ESP32 ne tourne pas ;
  - non nuls et crête > 0 : le son arrive.
- Câblage inchangé (vérifié : LED verte du module allumée, fils au bon endroit). Démarrage normal
  avec le module branché (GPIO45/46 non forcés).
- gestion_pubs.py inchangé. Compilé : 1 573 054 octets (50 %), aucun nouvel avertissement.

## v0.9.58 — validation sur le portail, APPRENDRE sûr / doute

**Portail → Capter → Fichiers de captation**
- Nouveau groupe **« Pubs apprises, à vérifier »** (pubs de la base marquées « à vérifier ») : ▶ écouter
  (`/pub/audio/N.wav`), ✓ valider (`CMD_GARDER`), ✕ supprimer (`CMD_SUPPRIMER`, son compris),
  « ✓ Tout valider » / « ✕ Tout supprimer » (`CMD_SUPPRIMER_A_VERIFIER`). Raison du doute affichée.
  API : `GET/POST /api/averifier` (op = ok, suppr, ok_tous, suppr_tous ; id).
- **Fichiers AUTO** : ▶ dans la page, ✓ = ajouter la pub (OK, chaîne de index.csv, son 8 kHz écrit dans
  /pub/audio, fichier effacé), ✕ comme avant. « 🔎 Dans la base ? » (✓ déjà n°X / ? ressemble n°X (p %) /
  nouvelle), « ✓ Valider les N nouvelles », « ✕ Effacer les N déjà dans la base ». API : `GET/POST /api/fi`
  (op = comparer, nouvelles, valider, un, suppr_connus ; f). Jamais pendant une captation.
- Empreintes d'un fichier sur la carte : tâche « fichiers » (cœur 1, priorité 1). Le moteur en direct attend
  sous `plat_emp_prendre()` (1 à 2 s par fichier) ; `emp::hors_ligne_debut/repartir/fin` sauvent et remettent
  **exactement** son état (vérifié sur PC : 27 411 codes identiques avec ou sans fichier au milieu, horloge
  inchangée). Mise en route de 2 s puis reprise au début du fichier : codes dès la 1re seconde. Le détecteur
  (seul à toucher la base) compare et ajoute (`CMD_FICHIER`). « Déjà dans la base » = score ≥ seuil et
  étendue ≥ 60 % du fichier et de la pub.
- Bas de page : pubs · à vérifier · détections · coupures, chaque seconde (`/api/etat` : `det`).

**APPRENDRE**
- Sûre → **OK tout de suite** : deux vrais silences, durée classique (10/15/20/25/30/40/45/60 s ± 1 s),
  ne ressemble à aucune pub connue (score < 6). Sinon « à vérifier » + raison (`drapeaux` bits 1-3 :
  1 pas de silence d'un côté, 2 durée inhabituelle, 3 ressemble ; gardés par gestion_pubs.py).
- Une pub apprise « à vérifier » (origine auto) entendue une 2e fois est **validée**.
- Bilan : « dont N sûre(s) ou revue(s) » (`/api/capter` : `su`).

gestion_pubs.py : aucun changement (numéro de version seulement). Compilé : 1 572 826 octets (49 %).

## v0.9.57 — découpe AUTO / APPRENDRE fiabilisée, bilan complet

Diagnostic (journal du 07/11 + cap_016.wav) : la découpe ne reposait que sur les silences. Sans silence
pendant 90 s, le morceau était jeté avec les pubs (connues et inconnues) qu'il contenait, et le bilan ne
le montrait pas. Sur cap_016.wav la découpe marche (16 morceaux) : le problème dépend du moment/chaîne.

- **Pubs reconnues = repères** : le détecteur donne la position dans la pub et sa durée
  (`Vue.pos_s`, `duree_s`, nouveau `Vue.col`) → coupe juste avant et juste après, même sans silence ;
  le morceau entre deux limites franches est rattrapé (APPRENDRE : depuis le journal ; AUTO : si encore dans l'anneau).
- **AUTO** : après 40 s sans vrai silence, coupe au premier creux doux (160 ms < 35 %) ; fichier `…_sans_silence.wav`.
  Les morceaux bornés par deux pubs reconnues sans silence portent aussi ce suffixe.
- **APPRENDRE** n'apprend que des morceaux sûrs d'être UNE pub : au moins une limite = vrai silence,
  5–60 s (deux silences) ou 5–45 s (un silence + une pub reconnue). Sinon : « Non appris ».
- **Filet APPRENDRE** : 20 s ou plus non traitées → `cap_NNN.wav` gardé (Captations entières), à traiter dans Python.
- Morceaux de moins de 1 s ignorés sans être comptés (plus de « trop courte 0,3 s »).
- **Bilan sur toute la durée** (portail, écran, console) : durée captée, apprises/enregistrées, connues entendues
  (avec numéros et durée), trop courtes, jetées, sans silence, non traité, captation gardée.
  JSON `/api/capter` : `cs`, `ns`, `sc`, `ga`, `kl`. 8 derniers verdicts au lieu de 5.
- **Bug corrigé** : « serveur web muet : relance du serveur » toutes les 2 min sans raison (l'auto-test de
  connexion à soi-même échoue toujours sur cette carte) → test vérifié une fois, désactivé s'il ne peut pas marcher.
- **Bug corrigé** : écran Capter, ligne des statistiques en direct tronquée (« · N jetées » coupé).
- Simulation PC de la nouvelle découpe sur cap_016.wav (aucune régression) et sur le cas « sans silences ».
- Compilé : 1 555 838 octets (49 %).

## v0.9.56 — gestion_pubs.py simplifié (2 modes), compteurs partout

**gestion_pubs.py** : plus de barre de menus. Écran d'accueil dans la fenêtre, deux colonnes :
- 💾 **MODE 1 : la carte SD est dans le PC** — « Ouvrir la carte SD » (cherchée seule sur les lecteurs ;
  si elle a des éléments de l'ancien format, mise à niveau proposée : copie complète ZIP puis effacement
  des seuls éléments oranges du nettoyage) · « Nettoyer la carte SD ».
- 🌐 **MODE 2 : par le portail (.zip)** — « Vérifier les pubs » · « Ajouter des pubs depuis des captations ».
  Le dernier .zip du portail est pris dans Téléchargements (base = contient base.bin, captations = .wav) ;
  s'il a déjà été ouvert, on demande (liste dans DetecteurPub/deja_ouverts.txt).
- Pendant le travail : bandeau du mode + chiffres ; gros boutons 🏠 Accueil · ⚠ Vérifier les pubs (N) ·
  🎙 Captations (mode 1 : celles de la carte ; mode 2 : le ZIP) · ✅ Enregistrer sur la carte / Terminer :
  préparer l'envoi · ⏏ Éjecter (mode 1) · ↶. Le reste dans « ⋯ Plus » (une liste, sans sous-menus).
  Outils de découpe repliés sous « ✂ Découper cette pub ».

**Compteurs identiques partout** : « N pubs · N à vérifier · N détections » (détections = somme des
passages reconnus des pubs, `base::nb_detections()`).
- Écran de la carte : la bande du bas n'est plus cachée par les messages (« Télé hors ligne »…) :
  toutes les 4 s, message → chiffres → date · version · vue · mémoire.
- Portail (Base complète) : `/api/base_info` ajoute `det`.
- Compilé : 1 551 886 octets (49 %).

## v0.9.55 — nettoyage d'une carte SD (gestion_pubs.py seulement, firmware inchangé = v0.9.54)

Fichier → « 🧹 Nettoyer une carte SD (branchée sur le PC) ». Détecteur éteint, carte SD dans le PC.
1. Analyse (n'écrit rien) : 🟢 gardé (base.bin, infos.bin, sons des pubs, voix/, motcle.bin, tv/, coupures*.csv,
   journal.csv, archive.ulaw, synchro.txt, 5 dernières sauv_NNN.pub + autres .pub) · 🟠 inutile, coché (base.new
   incomplet, base.new/infos.new entiers repris comme la carte au démarrage, sons sans pub, audio/*.tmp, _nouvelle/,
   ancienne_base/, import.pub, banque.pub, *.bin.pc, sdtest.bin, sauvegardes_pc/ → 5 plus récentes rangées dans
   DetecteurPub/sauvegardes, sauv_NNN.pub au-delà des 5 dernières, _en_cours.wav, _supprimees/) · 🔵 captations
   (AUTO, entières, test optique), non cochées · ⚪ inconnu (fichier ou dossier), non coché.
   Base illisible → aucun son proposé ; base vide → sons non cochés d'office.
2. « Nettoyer » : copie complète de pub/ (+ fichiers de la racine cochés) dans
   DetecteurPub/sauvegardes_carte_sd/sd_complete_<date>.zip, contrôle de chaque fichier, puis seulement les lignes cochées.
   3 copies complètes gardées. Dossiers système (System Volume Information…) jamais touchés.
3. « Remettre une copie complète sur la carte SD » (menu Carte SD) : pub/ remplacé par la copie.
Testé sur une fausse carte v0.9.43 (59 fichiers) : nettoyage, 2e analyse sans rien à cocher, restauration identique.

## v0.9.54 — une seule façon de travailler (portail + gestion_pubs.py)

La carte SD reste dans le détecteur : tout passe par le portail web.

**Gérer les pubs**
1. Portail → Réglages → Base complète → « ⬇ Carte → PC » (base_pub.zip).
2. gestion_pubs.py → « 📚 Ouvrir la base (ZIP) » : revue proposée s'il y a des pubs à vérifier.
3. « Enregistrer et préparer l'envoi » (Ctrl+S) : crée `DetecteurPub/a_envoyer/base_corrigee.zip`.
4. Portail → « ⬆ PC → carte », choisir base_corrigee.zip, « Remplacer la base ».

**Traiter des captations**
1. Portail → Capter → Fichiers de captation → groupe → « ⬇ Tout télécharger (ZIP) ».
2. gestion_pubs.py → « 🎙 Captations » (ouvre la base d'abord, puis le ZIP des captations).
   Un morceau AUTO est choisi en entier ; après « Ajouter comme pub », le fichier suivant s'ouvre.
   « Pas une pub : supprimer » efface le fichier.
3. Enregistrer et envoyer comme ci-dessus, puis « Vider » le groupe sur le portail.

**Règles fixes**
- PC : dossier `DetecteurPub` (base/, captations/, a_envoyer/, sauvegardes/). Une seule base et une
  seule captation en cours (remplacées à chaque ouverture). Sauvegardes : les 5 dernières.
- Avertissement si une base corrigée n'a pas encore été envoyée avant d'en ouvrir une nouvelle.
- Plus de dossiers `_supprimees` / `_travail` : une suppression est une vraie suppression.
- Carte : sauvegardes .pub limitées aux 5 dernières ; « Vider » par groupe de captations.
- Mêmes chiffres partout : portail (Base complète) « N pubs · N à vérifier · dernière base reçue du PC »,
  écran de la carte (pubs · à vérifier), gestion_pubs.py (pubs · à vérifier · base du …).
- L'ancien mode (carte SD branchée sur le PC) reste dans Fichier → « Carte SD branchée sur le PC ».

**Corrigé** : après un téléchargement ZIP interrompu, le suivant pouvait relire le fichier de l'ancien.

## v0.9.53 — captation APPRENDRE, base distante désactivée

**Nouveau bouton APPRENDRE** (portail web, section Captation, et onglet Capter de l'écran) :
- même découpe que AUTO (morceaux de 5 à 90 s coupés aux silences, pubs connues ignorées) ;
- chaque pub inconnue est **apprise directement dans la bibliothèque** : origine « auto »,
  chaîne choisie, marque « à vérifier », son dans `/pub/audio/N.wav` ;
- le son de la télé n'est jamais coupé pendant la captation ;
- aucun fichier gardé : pas de morceaux dans `/pub/captations/auto/`, et le bandeau entier
  `cap_NNN.wav` est supprimé à la fin (son numéro est rendu) ;
- ANNULER efface les fichiers mais **garde les pubs déjà apprises** ;
- bilan affiché à la fin (apprises, connues, trop courtes, jetées) ;
- bibliothèque pleine (512) : l'apprentissage s'arrête, la captation continue.
- Validation ensuite sur PC avec `gestion_pubs.py` (pubs « à vérifier », origine « auto »).

**Base distante désactivée** : `#define BASE_DISTANTE 0` dans `config.h`. Le code
(`distant.cpp`) est exclu de la compilation, la carte travaille seule, le portail n'affiche
plus « Base de pubs (réseau) ». Remettre 1 pour la retrouver.

Technique : nouvelle commande `det::CMD_APPRENDRE_AUTO` (passage en colonnes d'empreintes,
calculé depuis la position dans l'anneau audio, à ~64 ms près) ; `det::commande()` signale
maintenant une file pleine.

## v0.9.52

- Banc (v0.9.51) : copie PSRAM 300 Ko 13 ms, calcul sans lissage 26 ms, lissé 72 ms,
  envoi 19 ms. Sans lissage jugé pas beau → lissage gardé.
- **Mise à jour partielle** : seule la zone de l'interface qui a changé (comparée à la
  dernière image affichée) est recalculée et envoyée. Changement d'onglet = image entière.
  Vérifié sur PC : 3000 images aléatoires, écran identique au recalcul complet.
- Journal : `[ECRAN] image N : zone LxH, calcul X ms, envoi Y ms`.

## v0.9.51

- Mesure v0.9.50 : calcul 164–170 ms, envoi 54–61 ms (aucun gain du cœur 1).
- Banc d'essai au démarrage (`[ECRAN] banc : ...`) : copie PSRAM, calcul sans et avec
  lissage, envoi, mélange en RAM interne, mesurés séparément.
- Priorité haute pour la tâche d'écran pendant son calcul.

## v0.9.50

- Mesure v0.9.49 après 1000 images : calcul 175 ms, envoi 32 ms. Le calcul était
  interrompu par le moteur d'analyse (même cœur 0, priorité plus haute).
- Tâche d'écran déplacée sur le cœur 1 (celui de l'interface, au repos pendant l'envoi).
- Mélange des couleurs en une multiplication 32 bits par poids (au lieu de trois),
  résultat identique au pixel près (vérifié sur 20 millions de cas).

## v0.9.49

- Mesure v0.9.48 : calcul 80 ms, envoi 19 ms. Blocs de calcul portés de 8 à 32 lignes.
- Option `ECRAN_LISSAGE 0` (ecran_qspi.h) : agrandissement sans lissage, bien plus
  rapide, traits un peu irréguliers.

## v0.9.48

- Mesure sur la carte (v0.9.47) : calcul 127–185 ms, envoi 43–52 ms par image →
  écran lent, figé sur le Journal. Cause : l'image source était lue en colonne
  (un défaut de cache PSRAM par pixel). Calcul réorganisé par blocs de 8 lignes du
  panneau (même résultat au pixel près), envoi par bandes de 16 lignes au lieu de 8.

## v0.9.47

- Plus d'effet de « rideau » de droite à gauche en changeant d'onglet : l'image du
  panneau est entièrement calculée en PSRAM (300 Ko) avant d'être envoyée d'un trait.
- Le journal donne les temps réels : `[ECRAN] image N : calcul X ms, envoi Y ms`.

## v0.9.46 (deuxième essai)

- Image en miroir gauche-droite corrigée : paysage à l'endroit **micro à droite** ;
  `ECRAN_RETOURNE 1` pour le micro à gauche.
- Nouvel étalonnage du tactile demandé au démarrage (l'ancien avait été fait sur l'image en miroir).

## v0.9.45 (après le premier essai sur la carte)

- L'écran ignore le paysage matériel (MADCTL MV) : image coupée en deux et retournée.
  L'écran reste en portrait natif, la rotation se fait par logiciel. Paysage avec le
  micro à gauche ; `ECRAN_RETOURNE 1` = micro à droite ; `ECRAN_MIROIR 1` si le texte
  sort en miroir.
- Tactile introuvable : le ST7123 demande ~100 ms après son reset. Attente portée à
  150 ms + essais pendant 1 s ; en cas d'échec, la console liste les adresses I2C vues.

## Compatibilité ES3C28P → ES3C35P

| Élément | ES3C28P (2,8") | ES3C35P (3,5") | Effet sur le programme |
|---|---|---|---|
| Processeur | ESP32-S3 | ESP32-S3 N16R8 (16 Mo flash, 8 Mo PSRAM OPI) | identique |
| Écran | ILI9341 SPI 240×320 (TFT_eSPI) | ST77922 QSPI 320×480, IPS | **nouveau pilote** `ecran_qspi.cpp` |
| Rétroéclairage | GPIO45 | GPIO41 | broche |
| Tactile | FT6336 à 0x38 | Sitronix ST7123 à 0x55 (la notice dit FT6336 : faux) | **détecté automatiquement** |
| Bus I2C (tactile + codec) | 16 / 15 | 38 / 39 | broches |
| Codec audio | ES8311 à 0x18 | ES8311 à 0x18 | identique (même init, même gain) |
| I2S MCLK/BCLK/WS/DOUT/DIN | 4/5/7/8/6 | 17/18/21/15/16 | broches |
| Ampli haut-parleur | GPIO1 | GPIO1 (SC8002B, actif à l'état bas) | niveau détecté au 1er démarrage, comme avant |
| microSD 4 bits | 38/40/39/41/48/47 | CLK 5, CMD 4, D0-D3 6/7/2/3 | broches |
| LED RGB WS2812 | GPIO42 | GPIO40 | broche |
| Entrée optique TOSLINKBee | GPIO21/14/2 (P3) | **GPIO45/46 + GPIO44** | à recâbler (voir plus bas) |

## Ce qui a changé dans le code

- `ecran_qspi.cpp/.h` (nouveau) : écran ST77922 en QSPI à 40 MHz, séquence
  d'initialisation du fabricant, paysage 480×320. L'interface reste dessinée en
  320×240 et est agrandie ×4/3 avec lissage : 427×320 au centre, les deux bandes
  latérales prolongent le bord de l'image. Aucun écran de l'interface n'est à refaire.
- `ft6336_touch.cpp` v1.3 : essaie le FT6336 (0x38) puis le ST7123 (0x55).
- `pub_console.ino` : broches ES3C35P, plus de TFT_eSPI, rétroéclairage PWM à
  25 kHz (inaudible pour le micro), le tactile est relâché avant de créer le bus I2C.

## Réglages Arduino IDE

Paquet esp32 3.3.10, carte « ESP32S3 Dev Module », PSRAM « OPI PSRAM »,
Flash 16 Mo, partition « Huge APP » (ou « 16M Flash (3MB APP/9.9MB FATFS) »),
**USB CDC On Boot « Enabled »**. TFT_eSPI n'est plus utilisée.
Compilé : 1,55 Mo (49 % de la partition), 100 Ko de RAM globale.

## Premier démarrage

1. Si l'écran reste noir juste après le premier téléversement : débrancher et
   rebrancher la carte une fois (le RESET de l'écran est câblé sur celui de
   l'ESP32, un téléversement USB ne le remet pas à zéro).
2. Étalonnage du tactile : 3 croix à toucher (une seule fois, gardé en NVS).
3. Test de l'ampli (bip) : automatique, comme sur la 2,8".
4. Image à l'envers : mettre `#define ECRAN_RETOURNE 1` en tête de
   `ecran_qspi.h`, puis refaire l'étalonnage.

## Entrée optique (si utilisée)

Module confirmé : ToslinkBee V1.0, DIR9001 + CS8421 (sortie I2S 3,3 V, esclave : l'ESP32 fournit
BCK et LRCK, comme le projet de référence wavoigt/ESP32-Audio-Dynamic-Compressor).
Trous comptés depuis le côté opposé à la prise optique, module vu côté puces :

| Module TOSLINKBee | ES3C35P |
|---|---|
| Gauche 1er trou : J2-1 +5 V (entrée) | 5V du connecteur série P2 |
| Gauche 5e trou : J2-5 +3,3 V (SORTIE) | **ne rien brancher** |
| Gauche 6e trou : J2-6 GND (ou droite 4e/5e : J1-7/J1-6) | GND |
| Droite 1er trou : J1-10 LRCK | IO46 (connecteur d'extension) |
| Droite 2e trou : J1-9 SD (données) | RXD = GPIO44 (connecteur P2) |
| Droite 3e trou : J1-8 BCK | IO45 (connecteur d'extension) |

Avant la mise sous tension : continuité J2-1 ↔ entrée du régulateur 662K, et J2-6 ↔ J1-7.
Télé en sortie optique PCM, puis TEST FO.

## Points à vérifier sur la carte (pas testables sans le matériel)

- l'image et les couleurs de l'écran (séquence d'init relevée par d'autres sur ce panneau) ;
- le niveau du micro : capsule différente de la 2,8", le réglage de gain existant suffit normalement ;
- l'entrée optique sur GPIO45/46 (broches de configuration au démarrage, utilisées ici en sorties).

## Sources

- LCDwiki ES3C35P : https://www.lcdwiki.com/3.5inch_ESP32-S3_Display
- Relevés matériels (tactile 0x55, rotation, séquence d'init) : https://github.com/jlmeredith/ES3C35P
- Pilote ESP-IDF de cette carte : https://github.com/andrewscherpbier/esp32-s3-display
- Registres ST7123 : ESPHome, composant `st7123`
