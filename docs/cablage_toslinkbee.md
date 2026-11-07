# Câblage du module optique TOSLINKBee

Le module TOSLINKBee (Tinysine, V1.0, puce DIR9001) reçoit le son numérique de la **sortie optique** de la
télévision (S/PDIF) et le transmet à la carte en **I2S**. C'est optionnel : sans lui, la carte écoute avec son micro.

![Schéma des interconnexions](images/cablage_toslinkbee.svg)

## Les 6 points de connexion

Repères du module : trous comptés **depuis le côté opposé à la prise optique**, module vu **côté puces**.

| Module TOSLINKBee | Signal | Carte ES3C35P | Sens |
|---|---|---|---|
| Gauche, 1er trou : **J2-1** | +5 V (entrée d'alimentation) | **5V** du connecteur série **P2** | carte → module |
| Gauche, 5e trou : **J2-5** | +3,3 V (**sortie** du module) | **ne rien brancher** | — |
| Gauche, 6e trou : **J2-6** | GND | **GND** (P2 ou extension) | commun |
| Droite, 1er trou : **J1-10** | LRCK (horloge gauche / droite) | **IO46**, connecteur d'extension | carte → module |
| Droite, 2e trou : **J1-9** | SD (données audio) | **RXD = GPIO44**, connecteur **P2** | module → carte |
| Droite, 3e trou : **J1-8** | BCK (horloge des bits) | **IO45**, connecteur d'extension | carte → module |

Autres masses du module, si plus pratique : droite 4e et 5e trous (J1-7, J1-6).

## Où sont les broches sur l'ES3C35P

- **Connecteur série P2** (UART, 4 contacts, petit pas 1,25 mm) : **5V**, **GND**, **RXD** (GPIO44) et TXD (GPIO43,
  non utilisé ici).
- **Connecteur d'extension** : **IO45**, **IO46**, ainsi que 3,3 V et GND.
- Fiez-vous à la **sérigraphie de la carte** (noms imprimés à côté des contacts) et au schéma officiel LCDwiki,
  pas à l'ordre des fils ni à leur couleur : l'ordre des contacts peut varier selon les câbles fournis.

## Comment ça marche

- La carte est **maître** : c'est elle qui fabrique les deux horloges, **BCK** (3,072 MHz) et **LRCK** (48 kHz),
  sur son port I2S 1. Le module renvoie le son sur **SD**, au rythme de ces horloges.
- Chaque échantillon occupe une case de 32 bits ; LRCK doit être un carré 32 / 32 (réglage `ws_width = 32` dans
  `fo_regler_std()`, `pub_console.ino`). Sans ce réglage, le module ne trouve pas les voies et la carte ne lit que des zéros.
- La carte garde le mélange des deux voies (gauche + droite) / 2, puis réduit le son de 48 à 16 kHz.

## Avant la première mise sous tension

1. Carte **débranchée**.
2. Vérifier au multimètre (continuité) : J2-1 du module ↔ 5V de la carte, J2-6 ↔ GND.
3. Vérifier que **rien** n'est branché sur J2-5 (+3,3 V du module : c'est une sortie).
4. Vérifier qu'aucun fil ne touche un contact voisin.

Remarque : la consommation du module n'est pas documentée par le fabricant ; le 5 V de P2 vient de l'alimentation
de la carte. Sur le prototype, alimenté par l'USB, l'ensemble fonctionne sans problème.

## Premier essai

1. Télévision : **sortie audio optique en PCM** (pas Dolby, pas Bitstream, pas Auto).
2. Volume de la télé à **1 au moins** : sur certaines télés (dont la TCL), le volume 0 coupe aussi la sortie optique.
3. Brancher la fibre, alimenter la carte : la LED verte du module doit s'allumer.
4. Choisir la source **Optique** (Réglages → Détection, ou le préréglage « Optique confort »).
5. Lancer **TEST FO** (portail : Réglages → « Vérifier l'entrée optique »). Il enregistre 10 s dans
   `/pub/captations/fo_test_16k.wav` et donne un bilan dans la console.

## Ce qu'affiche la carte (bandeau du haut)

| Affichage | Sens | Que faire |
|---|---|---|
| **OPTIQUE** (vert) | le son de la fibre arrive | rien, tout va bien |
| **Optique: silence** | le module tourne mais n'envoie que des zéros | télé en PCM ? volume ≥ 1 ? fibre bien enfoncée ? LED verte du module ? |
| **Optique: démarrage** (rouge) | l'entrée optique ne démarre pas, le micro écoute encore | vérifier les fils BCK / LRCK / SD et le 5 V ; lire la raison dans la console (`[FO] …`) |
| **Optique: test** | TEST FO en cours | attendre 10 s |

Dans la console, en mode optique, une ligne toutes les 5 s puis toutes les minutes :
`[FO] 60 s : 2880000 échantillons (attendu 2880000), 2749057 non nuls, crête … , lectures vides 0`

- échantillons ≈ attendu **et** non nuls > 0 : le son arrive ;
- échantillons ≈ attendu mais **0 non nul** : les horloges marchent, le module n'envoie que des zéros (télé, PCM, fibre) ;
- beaucoup moins qu'attendu ou « lectures vides » : l'I2S de la carte ne tourne pas (câblage BCK / LRCK, réglage).

## Sources

- Module : https://www.tinysineaudio.com/products/toslinkbee-spdif-to-i2s-digital-audio-receiver-module
- Carte ES3C35P (schéma, manuel) : https://www.lcdwiki.com/3.5inch_ESP32-S3_Display
- Exemple ESP32 avec ce module : https://github.com/wavoigt/ESP32-Audio-Dynamic-Compressor
