// portail.h - la page web du telephone ou du PC, servie par l'ESP32
// (http://detecteur-pub.local). Une seule page, sans dependance externe :
//   Direct   : etat en direct, TESTER, ANNULER, coupure du son (temps, volume)
//   Tele     : telecommande Wi-Fi (IP, appairage, code, essais, oubli)
//   Reglages : tous les reglages, Wi-Fi de la box
//   Capter   : captation du micro, test de l'entree optique, fichiers de captation (v0.6.9)
//   Journal  : console de la carte (marche / arret), fichiers de la carte SD
#pragma once

static const char PAGE_PORTAIL[] = R"PAGE(<!doctype html>
<html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#060a1e">
<title>Détecteur de pub</title>
<style>
:root{--fond:#060a1e;--fond2:#10163a;--carte:#141b44;--carte2:#1c2558;--bord:#34428a;--txt:#ecf2ff;--txt2:#9ca8d2;--txt3:#606c9c;
--mag:#ff40c8;--rouge:#ff4e6a;--vert:#3cff9c;--cyan:#28e4ff;--jaune:#ffd440;--orange:#ff8c3c;--violet:#8c5cff;--r:16px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:linear-gradient(180deg,var(--fond),#10093a) fixed;color:var(--txt);font:15px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,sans-serif}
main{max-width:720px;margin:0 auto;padding:12px 14px 96px}
h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:var(--txt3);margin:20px 4px 8px}
.carte{background:var(--carte);border:1px solid var(--bord);border-radius:var(--r);padding:14px;margin-bottom:12px}
.ligne{display:flex;align-items:center;gap:10px}.grow{flex:1}.wrap{flex-wrap:wrap}.g2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.petit{font-size:12.5px;color:var(--txt2)}.tres{font-size:11.5px;color:var(--txt3)}
button{font:inherit;border:0;border-radius:14px;padding:12px 14px;font-weight:700;cursor:pointer;background:var(--carte2);color:var(--txt);transition:transform .06s}
button:active{transform:scale(.96);filter:brightness(1.25)}button.pris{transform:scale(.96);filter:brightness(1.4);box-shadow:inset 0 3px 8px rgba(0,0,0,.45),0 0 0 2px var(--cyan)}button[disabled]{opacity:.4}
.b-ok{background:linear-gradient(135deg,var(--vert),var(--cyan));color:#06061a}.b-pub{background:linear-gradient(135deg,var(--mag),var(--rouge));color:#06061a}
.b-j{background:linear-gradient(135deg,var(--jaune),var(--orange));color:#06061a}.b-v{background:linear-gradient(135deg,#5a46dc,#9646e6);color:#fff}
.b-danger{background:transparent;border:1px solid var(--rouge);color:var(--rouge)}.b-mini{padding:7px 10px;font-size:13px;border-radius:10px}
input,select{font:inherit;color:var(--txt);background:var(--fond2);border:1px solid var(--bord);border-radius:12px;padding:10px 12px;width:100%}
#bandeau{border-radius:22px;padding:16px 18px;border:2px solid var(--c,var(--vert));background:radial-gradient(120% 140% at 0% 0%,color-mix(in srgb,var(--c,var(--vert)) 32%,transparent),var(--carte) 70%)}
#mot{font-size:40px;font-weight:800;letter-spacing:-.02em;line-height:1}#sous{margin-top:6px;color:var(--txt2)}
.tester{display:flex;align-items:center;gap:16px;margin:14px 0}
#b-tester{position:relative;width:118px;height:118px;border-radius:50%;flex:none;background:radial-gradient(circle at 35% 30%,#7ff0ff,var(--cyan) 45%,#138fd1);color:#06061a;font-size:16px;box-shadow:0 0 30px #28e4ff55}
#b-tester.ecoute::before,#b-tester.ecoute::after{content:"";position:absolute;inset:-6px;border-radius:50%;border:3px solid var(--cyan);animation:onde 1.6s infinite ease-out}
#b-tester.ecoute::after{animation-delay:.8s}#b-tester.enreg{background:radial-gradient(circle at 35% 30%,#fff3b0,var(--jaune) 45%,var(--orange))}
#b-tester.enreg::before,#b-tester.enreg::after{border-color:var(--jaune)}#b-tester.connue{background:radial-gradient(circle at 35% 30%,#c8ffe4,var(--vert) 50%,#18b06a)}
@keyframes onde{from{transform:scale(1);opacity:.9}to{transform:scale(1.6);opacity:0}}
.barre{height:10px;border-radius:5px;background:var(--fond2);overflow:hidden;margin:10px 0 6px}.barre i{display:block;height:100%;background:var(--mag);transition:width .9s linear}
.inter{position:relative;width:56px;height:32px;flex:none}.inter input{display:none}
.inter span{position:absolute;inset:0;border-radius:20px;background:var(--fond2);border:1px solid var(--bord);transition:.2s}
.inter span:after{content:"";position:absolute;width:24px;height:24px;left:3px;top:3px;border-radius:50%;background:var(--txt2);transition:.2s}
.inter input:checked+span{background:color-mix(in srgb,var(--vert) 40%,var(--fond2));border-color:var(--vert)}.inter input:checked+span:after{left:27px;background:var(--vert)}
.pub{display:flex;gap:10px;align-items:center;padding:10px 12px;border-radius:14px;background:var(--carte);border:1px solid var(--bord);margin-bottom:8px}
.pub.verif{border-color:var(--jaune)}.num{flex:none;min-width:44px;text-align:center;font-weight:700;font-size:13px;padding:4px 6px;border-radius:10px;background:var(--carte2)}
nav{position:fixed;left:0;right:0;bottom:0;background:rgba(16,22,52,.95);border-top:1px solid var(--bord);display:flex;justify-content:space-around;padding:6px 6px calc(6px + env(safe-area-inset-bottom));z-index:5}
nav button{background:transparent;color:var(--txt3);font-size:12px;font-weight:600;padding:8px 6px}nav button.on{color:var(--cyan)}
section{display:none}section.on{display:block}
#toast{position:fixed;left:50%;top:12px;transform:translate(-50%,-90px);transition:.3s;background:var(--carte2);border:1px solid var(--cyan);padding:10px 16px;border-radius:14px;z-index:9;max-width:92vw}
#toast.on{transform:translate(-50%,0)}
.lab{font-size:12.5px;color:var(--txt2);margin:12px 0 6px}.lab:first-child{margin-top:0}
.jr{font:12px/1.35 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;max-height:60vh;overflow:auto;background:var(--fond2);border-radius:12px;padding:10px}
.jr .e{color:var(--rouge)}.jr .w{color:var(--orange)}.jr .t{color:var(--cyan)}.jr .s{color:var(--txt3)}
a{color:var(--cyan)}
code{background:var(--fond2);padding:1px 6px;border-radius:6px}
.pastille{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--txt3);margin-right:6px}
#ec .ecl{display:flex;align-items:center;gap:10px;padding:10px 2px;font-size:16px;line-height:1.35;cursor:pointer;border-top:1px solid var(--bord)}
#ec .ecl:first-child{border-top:0;padding-top:2px}#ec .ecl b{flex:none;min-width:104px}#ec .ecl .pastille{flex:none;width:12px;height:12px;margin-right:0}
#compteurs{font-size:17px;text-align:center;line-height:1.5}#compteurs b{font-size:20px}
.prg{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}.prb{border:1px solid var(--bord);border-radius:14px;padding:12px 6px;text-align:center;background:var(--fond2);color:var(--txt);font-size:15px;font-weight:700;line-height:1.25}
.prb small{display:block;font-weight:400;font-size:12px;color:var(--txt2);margin-top:3px}.prb.on{border:2px solid var(--vert);background:color-mix(in srgb,var(--vert) 16%,var(--fond2))}.prb .ic{font-size:22px;display:block;margin-bottom:4px}
@media(max-width:560px){.prg{grid-template-columns:1fr 1fr}}
</style></head><body>
<div id="toast"></div>
<div id="volchip" style="position:fixed;top:8px;right:10px;z-index:6;background:rgba(16,22,52,.92);border:1px solid var(--bord);border-radius:12px;padding:4px 10px;font-size:13px;font-weight:700;display:none"></div>
<main>
<!-- ======================= DIRECT ======================= -->
<section id="s-direct" class="on">
  <div id="bandeau">
    <div class="ligne" style="align-items:flex-start;gap:12px"><div class="grow"><div id="mot">…</div><div id="sous"></div></div>
      <div style="text-align:center;flex:none"><div class="petit" style="margin-bottom:4px"><b>Détecteur</b></div>
        <label class="inter" style="display:inline-block"><input type="checkbox" id="actif" onchange="regler('actif',this.checked?1:0)"><span></span></label>
        <div class="petit" id="actif-txt" style="margin-top:2px">en marche</div></div></div>
    <div class="ligne" onclick="onglet('tele')" title="Ouvrir l'onglet Télé" style="margin-top:10px;padding-top:8px;border-top:1px solid color-mix(in srgb,var(--c,var(--vert)) 30%,transparent);cursor:pointer;font-size:14px">
      <span class="pastille" id="tv-past"></span><span class="grow" id="tv-ligne">Télé : …</span><span class="petit">Télé ›</span></div>
  </div>
  <div class="carte" id="mem-alerte" style="display:none;margin-top:12px;border-color:var(--rouge);color:var(--rouge);font-weight:700;font-size:17px">⚠ Attention : surcharge de la mémoire interne</div>
  <div class="tester" style="margin-top:12px">
    <button id="b-tester" onclick="tester()">TESTER</button>
    <div class="grow"><b id="t-titre">Une pub passe ?</b><div class="petit" id="t-sous">TESTER : la carte la reconnaît, ou l'apprend si elle est nouvelle.</div>
      <div class="ligne wrap" style="margin-top:8px;gap:8px">
        <button class="b-j b-mini" id="b-annuler" style="display:none" onclick="cmd('annuler')">ANNULER</button>
        <button class="b-danger b-mini" id="b-paspub" style="display:none" onclick="cmd('paspub')">Ce n'est pas une pub</button></div></div>
  </div>
  <div class="carte" id="coupure" style="display:none;border-color:var(--mag)">
    <div class="ligne"><b class="grow" id="c-titre">SON BAISSÉ</b><b id="c-reste"></b></div>
    <div class="barre"><i id="c-barre" style="width:0"></i></div>
    <div class="ligne"><span class="petit grow" id="c-vol"></span><button class="b-ok b-mini" onclick="cmd('son')">Remettre le son</button></div>
  </div>
  <div class="carte" id="msg" style="display:none"><b id="msg-t"></b><div class="petit" id="msg-d"></div></div>
  <div class="carte" id="compteurs" style="margin-top:12px">…</div>
  <div class="carte"><div class="lab" style="margin:0 0 8px">PRÉRÉGLAGES · un appui suffit</div><div class="prg" id="pr-acc"></div></div>
  <div class="carte" id="ec"><div class="lab" style="margin:0 0 6px" id="ec-titre">EN CE MOMENT</div>
    <div class="ecl" onclick="onglet('reglages')"><span class="pastille" id="ec-p0"></span><b>🔊 Source</b><span class="grow" id="ec-t0">…</span></div>
    <div class="ecl" onclick="onglet('reglages')"><span class="pastille" id="ec-p1"></span><b>🔎 Détection</b><span class="grow" id="ec-t1">…</span></div>
    <div class="ecl" onclick="onglet('reglages')"><span class="pastille" id="ec-p2"></span><b>🔇 Coupure</b><span class="grow" id="ec-t2">…</span></div>
    <div class="ecl" onclick="onglet('capter')"><span class="pastille" id="ec-p3"></span><b>🎙 Captation</b><span class="grow" id="ec-t3">…</span></div>
    <div class="ecl" onclick="onglet('journal')"><span class="pastille" id="ec-p4"></span><b>🧠 Mémoire</b><span class="grow" id="ec-t4">…</span></div>
  </div>
  <div class="carte" id="sp-carte"><div class="ligne" style="margin-bottom:6px"><div class="lab grow" style="margin:0">SPECTRE DU SON</div><span class="petit" id="sp-etat"></span></div>
    <canvas id="sp" width="480" height="96" style="width:100%;height:96px;display:block;border-radius:10px;background:#06061a"></canvas>
    <div class="ligne petit" style="margin-top:4px"><span class="grow">graves en bas, aigus en haut · défile de droite à gauche</span><span>≈ 2 min</span></div></div>
  <div class="carte" id="cps" style="display:none">
    <details id="cps-d" open><summary style="cursor:pointer"><b>Dernières coupures du son</b> <span class="petit" id="cps-r"></span></summary>
    <div id="cps-l" style="margin-top:8px"></div>
    <div class="petit" style="margin-top:6px">Une coupure à tort ? « Fausse alerte » supprime la pub qui l'a déclenchée de la bibliothèque.</div></details>
  </div>
  <div class="carte" id="plantage" style="display:none;margin-top:18px;border-color:var(--rouge);color:var(--rouge);font-size:17px;line-height:1.45;padding:16px"><div id="plantage-txt"></div><button class="b-mini" style="margin-top:12px;padding:10px 18px" onclick="plantageEff()">Effacer ce message</button></div>
  <div class="petit" id="heure-carte" style="margin-top:14px;text-align:center"></div>
  <p class="tres" id="pied"></p>
</section>

<!-- ======================= TELE ======================= -->
<section id="s-tele">
  <h2>Télécommande Wi-Fi</h2>
  <div class="carte">
    <div class="ligne"><span class="pastille" id="tv-past2"></span><b class="grow" id="tv-etat">…</b></div>
    <div class="petit" id="tv-vol" style="margin-top:6px"></div>
    <div class="petit" id="tv-msg" style="margin-top:6px"></div>
  </div>
  <div class="carte">
    <div class="lab">1. Adresse IP de la télé (box → appareils connectés, ou télé → Paramètres → Réseau)</div>
    <div class="ligne"><input id="tv-ip" inputmode="decimal" placeholder="192.168.1.55"><button class="b-mini" onclick="tvIp()">OK</button></div>
    <div class="lab">2. Appairage (une seule fois) : allume la télé, un code s'affiche</div>
    <div class="ligne wrap" style="gap:8px"><button class="b-v" id="b-app" onclick="tv('appairer')">Appairer</button>
      <button class="b-mini" id="b-app-ann" style="display:none" onclick="tv('annuler')">Annuler</button></div>
    <div id="code-zone" style="display:none;margin-top:10px">
      <div class="lab">Code affiché sur la télé (6 caractères 0-9 A-F) · <span id="code-reste"></span></div>
      <div class="ligne"><input id="tv-code" maxlength="6" autocapitalize="characters" placeholder="A1B2C3" style="text-transform:uppercase;letter-spacing:.2em;font-weight:700"><button class="b-ok" onclick="tvCode()">Envoyer</button></div>
    </div>
  </div>
  <h2>3. Essais</h2>
  <div class="carte g2">
    <button class="b-pub" onclick="tv('couper')">Couper<br><small>→ niveau bas</small></button>
    <button class="b-j" onclick="tv('diminuer')">Diminuer<br><small>un cran</small></button>
    <button class="b-ok" onclick="tv('augmenter')">Augmenter<br><small>un cran</small></button>
    <button class="b-ok" onclick="tv('retablir')">Rétablir<br><small id="r-sous">niveau haut</small></button>
    <button class="b-v" onclick="tv('lire')">Lire<br><small>le volume</small></button>
  </div>
  <div class="carte"><b>Oublier la télé</b><div class="petit">Efface la clé de la carte SD. Retire aussi « Detecteur de pub » des télécommandes dans les paramètres de la télé.</div>
    <button class="b-danger b-mini" style="margin-top:8px" onclick="if(confirm('Oublier la télé ? Il faudra réappairer.'))tv('oublier','&confirme=1')">Oublier la télé</button></div>
</section>

<!-- ======================= CAPTER ======================= -->
<section id="s-capter">
  <h2>Capter les pubs</h2>
  <div class="carte">
    <div class="petit">Lance une captation pendant un écran de pub. Le son de la télé n'est jamais baissé pendant une captation.</div>
    <div class="ligne" style="margin-top:10px"><span class="petit">Chaîne regardée</span><select id="cap-ch" class="grow" onchange="capCh(this.value)"></select></div>
    <div id="blk-appr" style="margin-top:12px">
      <button class="b-j" id="b-cap-appr" style="width:100%;padding:16px;font-size:17px" onclick="capterAppr()">🧠 APPRENDRE les nouvelles pubs</button>
      <div class="petit" style="margin-top:4px">La carte apprend toute seule chaque pub inconnue. Les pubs douteuses arrivent dans « Pubs apprises, à vérifier » (plus bas) : ▶ écouter, ✓ garder, ✕ supprimer.</div>
    </div>
    <div id="blk-auto" style="margin-top:12px">
      <button class="b-v" id="b-cap-auto" style="width:100%;padding:16px;font-size:17px" onclick="capterAuto()">✂ DÉCOUPER les pubs en fichiers</button>
      <div class="petit" style="margin-top:4px">Chaque pub inconnue (5 à 90 s) est mise dans un fichier, sans l'apprendre. Tu choisis ensuite dans « Fichiers de captation » : ▶ ✓ ✕.</div>
    </div>
    <div style="margin-top:12px">
      <button class="b-ok" id="b-cap" style="width:100%;padding:16px;font-size:17px" onclick="capter()">⏺ TOUT ENREGISTRER</button>
      <div class="petit" id="cap-aide" style="margin-top:4px">Enregistre tout le son, 15 min au plus, en un seul fichier. À découper ensuite sur le PC avec gestion_pubs.py.</div>
    </div>
    <button class="b-danger b-mini" id="b-cap-ann" style="display:none;width:100%;margin-top:8px;padding:12px" onclick="capAnnuler()">ANNULER la captation (supprime tout)</button>
    <div class="petit" id="cap-info" style="margin-top:8px"></div>
  </div>
  <div class="carte" id="cap-stats" style="display:none">
    <b id="cs-titre">Statistiques (en direct)</b>
    <div id="cs-cpt" style="display:grid;grid-template-columns:repeat(2,1fr);gap:6px 12px;margin-top:8px;font-size:14px"></div>
    <div id="cs-ev" style="margin-top:10px;font-size:14px"></div>
  </div>
  <h2>Fichiers de captation</h2>
  <div class="carte"><div id="av-liste" class="petit"></div><div id="cap-fichiers" class="petit">…</div><audio id="lecteur" preload="none" style="display:none"></audio></div>
</section>

<!-- ======================= REGLAGES ======================= -->
<section id="s-reglages">
  <h2>Préréglages</h2>
  <div class="carte">
    <div class="petit">Un préréglage garde : source du son, coupure, volume pendant la pub, durée d'une coupure, après la pub, sensibilité, gain du micro, réécoute. Il ne touche ni à la télé, ni au Wi-Fi, ni à l'écran.</div>
    <div class="lab">Enregistrer les réglages actuels dans :</div>
    <div class="ligne wrap" id="pr-cases" style="gap:8px"></div>
    <div class="ligne" style="margin-top:10px;gap:8px"><span class="petit">Nom</span><input id="pr-nom" maxlength="19" style="flex:1" placeholder="Soirée film"><button class="b-ok b-mini" onclick="prEnr()">Enregistrer</button></div>
    <div class="ligne" style="margin-top:12px"><span class="grow"></span><button class="b-danger b-mini" onclick="prOrigine()">Remettre les préréglages d'origine</button></div>
  </div>
  <h2>Son de la télé</h2>
  <div class="carte">
    <div class="lab">Volume pendant la pub (0 = silence)</div><input type="number" min="0" max="100" data-c="tv_bas">
    <div class="lab">Après la pub</div><select data-c="tv_origine"><option value="1">revenir au volume d'avant</option><option value="0">niveau haut fixe</option></select>
    <div class="lab">Niveau haut</div><input type="number" min="0" max="100" data-c="tv_haut">
    <div class="lab">Coupure</div><select data-c="coupure"><option value="1">par fenêtres (le son remonte pour réécouter)</option><option value="0">suivre la pub (niveau bas audible)</option></select>
    <div class="lab">Durée d'une coupure (s)</div><input type="number" min="10" max="240" step="5" data-c="coupure_s">
    <div class="petit">Entrée optique : le son n'est pas remonté pour réécouter. Il reste baissé pendant toute la pub ; à la fin de l'écran il remonte en 4 paliers (6 s après la dernière pub reconnue, lentement puis plus vite) et retrouve le volume d'avant la pub après cette durée sans pub reconnue (12 min au plus). Volume pendant la pub : au moins 1 (sur certaines télés, 0 coupe aussi la sortie optique). Les pubs inconnues passées entre deux pubs reconnues sont apprises (« à vérifier »).</div>
    <div class="lab">Fin de la coupure</div><select id="finspot" ><option value="1">automatique (le son revient à la fin du spot reconnu)</option><option value="0">imposée (toujours la durée réglée)</option></select>
    <div class="lab">Réécoute après le retour du son</div><select data-c="reecoute"><option value="0">3 s</option><option value="1">5 s</option><option value="2">8 s</option><option value="3">12 s</option></select>
  </div>
  <h2>Détection</h2>
  <div class="carte">
    <div class="lab">Sensibilité</div><select data-c="seuil"><option value="0">souple</option><option value="1">normale</option><option value="2">stricte</option></select>
    <div class="lab">Source du son</div><select data-c="source"><option value="0">micro de la carte</option><option value="1">entrée optique (TOSLINKBee)</option></select>
    <div class="lab">Gain du micro</div><select data-c="gain"><option value="0">automatique</option><option value="1">+12 dB</option><option value="2">+21 dB</option><option value="3">+30 dB</option><option value="4">+36 dB</option><option value="5">+42 dB</option><option value="6">+48 dB</option></select>
)PAGE"
#if BASE_DISTANTE
R"PAGE(    <div class="lab">Base de pubs (réseau)</div><select data-c="base"><option value="0">carte seule</option><option value="1">base décentralisée</option><option value="2">les deux</option></select>
    <div class="lab">Adresse de la base (vide = trouvée toute seule)</div><input data-c="serveur" data-t="1" placeholder="192.168.1.20">
)PAGE"
#endif
R"PAGE(  </div>
  <h2>Vérifier l'entrée optique</h2>
  <div class="carte">
    <button class="b-mini" id="b-fo" style="width:100%;padding:16px;font-size:17px" onclick="testFo()">📡 TEST FO (10 s)</button>
    <div class="petit" style="margin-top:4px">Écoute 10 s la fibre optique et dit si le son arrive (fichier fo_test_16k.wav sur la carte SD). Utile si le bandeau affiche « Optique: démarrage » ou « Optique: silence ».</div>
    <div class="petit" id="fo-info" style="margin-top:8px"></div>
  </div>
  <h2>Carte</h2>
  <div class="carte">
    <div class="lab">Bip du haut-parleur</div><select data-c="bip"><option value="0">non</option><option value="1">début de pub</option><option value="2">début et fin</option></select>
    <div class="lab">Luminosité (%)</div><input type="number" min="5" max="100" step="5" data-c="lum">
    <div class="lab">Écran</div><select data-c="ecran"><option value="0">toujours allumé</option><option value="1">discret (s'éteint après 1 min)</option></select>
    <div class="lab">LED témoin</div><select data-c="led"><option value="0">éteinte</option><option value="1">état</option></select>
    <div class="lab">Journal en mémoire</div><select data-c="journal"><option value="1">gardé</option><option value="0">coupé (moins de travail)</option></select>
  </div>
  <button class="b-ok" style="width:100%" onclick="envoyerConfig()">Envoyer à la carte</button>
  <h2>Wi-Fi de la box</h2>
  <div class="carte">
    <div class="petit" id="wifi-etat"></div>
    <div class="lab">Réseau</div><input id="w-ssid" autocomplete="off">
    <div class="lab">Mot de passe</div><input id="w-mdp" type="password">
    <button class="b-mini" style="margin-top:10px" onclick="wifi()">Se connecter</button>
  </div>
  <h2>Bibliothèque de pubs</h2>
  <div class="carte">
    <div class="petit">Une sauvegarde (.pub) garde les empreintes de toutes les pubs, sans leur son. Restaurer ajoute les pubs d'une sauvegarde à la bibliothèque. Les 5 dernières sauvegardes sont gardées dans /pub/sauvegardes/.</div>
    <div class="ligne wrap" style="gap:8px;margin-top:10px">
      <button class="b-ok b-mini" onclick="sauvegarder()">Sauvegarder maintenant</button>
      <label class="b-mini" style="background:var(--carte2);border-radius:10px;padding:7px 10px;font-weight:700;cursor:pointer">Restaurer un fichier .pub<input type="file" accept=".pub" style="display:none" onchange="importer(this)"></label>
      <button class="b-danger b-mini" onclick="effacer()">Tout effacer</button>
    </div>
    <div class="petit" id="sauv-msg" style="margin-top:8px;color:var(--vert)"></div>
    <div id="sauvs" style="margin-top:10px"></div>
  </div>
  <h2>Base complète (PC)</h2>
  <div class="carte">
    <div class="petit">Rapatrie sur ton PC toute la base : les pubs, leurs sons et leurs fiches, en un fichier ZIP. Corrige-la avec gestion_pubs.py, puis renvoie-la ici : elle <b>remplace entièrement</b> la base de la carte (l'ancienne est gardée dans /pub/ancienne_base/).</div>
    <div id="bz-resume" style="margin-top:8px;font-weight:700"></div>
    <div class="petit" id="bz-info" style="margin-top:4px"></div>
    <button class="b-ok" style="width:100%;margin-top:10px;padding:12px" id="bz-dl" onclick="baseTelecharger()">⬇ Carte → PC : télécharger toute la base</button>
    <div class="ligne wrap" style="gap:8px;margin-top:10px">
      <label class="b-mini" style="background:var(--carte2);border-radius:10px;padding:10px 14px;font-weight:700;cursor:pointer;flex:1;text-align:center">⬆ PC → carte : envoyer la base corrigée (.zip)<input type="file" accept=".zip" style="display:none" onchange="baseEnvoyer(this)"></label>
    </div>
    <div class="barre" id="bz-barre" style="display:none"><i id="bz-i" style="width:0"></i></div>
    <div class="petit" id="bz-txt" style="margin-top:4px"></div>
    <button class="b-danger" id="bz-stop" style="display:none;width:100%;margin-top:8px;padding:12px" onclick="baseStop()">Arrêter le transfert</button>
    <div class="ligne wrap" style="gap:8px;margin-top:10px"><button class="b-mini" onclick="baseDebit()">Mesurer les débits (réseau / SD)</button></div>
    <div class="petit" id="bz-deb" style="margin-top:6px"></div>
    <div id="bz-conf" style="display:none;margin-top:10px">
      <div id="bz-conf-txt" style="font-weight:700;margin-bottom:8px"></div>
      <div class="ligne wrap" style="gap:8px"><button class="b-danger b-mini" onclick="baseAppliquer()">Remplacer la base</button><button class="b-mini" onclick="baseAnnuler()">Annuler</button></div>
    </div>
  </div>
  <h2>Redémarrage</h2>
  <div class="carte">
    <div class="petit">Redémarre le programme de la carte (comme le bouton reset, sans toucher à la SD ni aux réglages). Utile si le portail ou l'écran se fige. Impossible pendant une captation.</div>
    <button class="b-danger" style="width:100%;margin-top:10px;padding:14px" onclick="redemarrer()">Redémarrer la carte</button>
    <div class="petit" id="rb-info" style="margin-top:8px"></div>
  </div>
  <h2>Écriture sur la carte SD</h2>
  <div class="carte">
    <div class="petit">Taille du tampon d'écriture (mémoire interne de la carte). Plus grand : écriture plus rapide. Plus petit : libère de la mémoire si la liaison avec la télé devient instable. Appliqué au <b>prochain démarrage</b> de la carte.</div>
    <div class="ligne" style="margin-top:10px"><span class="petit">Tampon</span>
      <select id="sdbuf" class="grow" onchange="sdbufEnv(this.value)"><option value="4">4 Ko (économe)</option><option value="8">8 Ko</option><option value="16">16 Ko (rapide)</option></select></div>
    <div class="petit" id="sdbuf-info" style="margin-top:8px"></div>
  </div>
</section>

<!-- ======================= JOURNAL ======================= -->
<section id="s-journal">
  <div class="carte ligne"><div class="grow"><b>Journal de la carte</b><div class="petit" id="jr-etat"></div></div>
    <label class="inter"><input type="checkbox" id="jr-on" onchange="regler('journal',this.checked?1:0)"><span></span></label></div>
  <div class="ligne" style="gap:8px;margin-bottom:8px"><button class="b-mini" id="jr-pause" onclick="jrPause=!jrPause;this.textContent=jrPause?'Reprendre':'Pause'">Pause</button>
    <button class="b-mini" onclick="navigator.clipboard&&navigator.clipboard.writeText(document.getElementById('jr').innerText).then(()=>toast('Copié'))">Copier</button></div>
  <div class="jr" id="jr"></div>
  <h2>Carte SD</h2>
  <div class="carte"><div class="petit" id="sd-info"></div><div id="sd" style="margin-top:8px"></div>
    <button class="b-mini" style="margin-top:8px" onclick="post('/api/sd/nettoyer').then(t=>{toast(t);sd()})">Nettoyer les sons sans pub</button></div>
</section>
</main>
<nav>
  <button class="on" data-o="direct" onclick="onglet('direct')">Direct</button>
  <button data-o="tele" onclick="onglet('tele')">Télé</button>
  <button data-o="capter" onclick="onglet('capter')">Capter</button>
  <button data-o="reglages" onclick="onglet('reglages')">Réglages</button>
  <button data-o="journal" onclick="onglet('journal')">Journal</button>
</nav>
<script>
const $=id=>document.getElementById(id);
let bzOcc=false,E={},vuMsg=0,vuTv=0,ong='direct',jrDer=0,jrPause=false,cfgLue=false;
const COUL=['#787f9c','#8c5cff','#3cff9c','#ff8c3c','#ff40c8','#ffd440','#787f9c'];
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('on');clearTimeout(e.t);e.t=setTimeout(()=>e.classList.remove('on'),3500)}
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b||''}).then(r=>r.text())}
let att=null;
function cmd(c){if(c=='tester'||c=='annuler'||c=='enregistrer'){att={q:c,test:(typeof E!='undefined'&&E)?E.test:0,t:Date.now()};$('t-titre').textContent=c=='enregistrer'?"J'enregistre la pub…":(c=='annuler'?"J'annule…":'Je commence…');$('t-sous').textContent='un instant'}post('/api/cmd','c='+c);setTimeout(etat,250)}
function tester(){cmd(E.test==4?'enregistrer':(E.test==1?'annuler':'tester'))}
function tv(a,x){post('/api/tv','a='+a+(x||'')).then(t=>{if(t!='…')toast(t)});setTimeout(etat,300)}
function tvIp(){post('/api/tv','a=ip&v='+encodeURIComponent($('tv-ip').value.trim())).then(toast);setTimeout(etat,300)}
function tvCode(){tv('code','&v='+encodeURIComponent($('tv-code').value.trim().toUpperCase()));$('tv-code').value=''}
function regler(k,v){post('/api/regler','cle='+k+'&v='+v).then(()=>{cfgLue=false});setTimeout(etat,250)}
function onglet(o){ong=o;document.querySelectorAll('section').forEach(s=>s.classList.toggle('on',s.id=='s-'+o));
 document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('on',b.dataset.o==o));window.scrollTo(0,0);
 try{post('/api/ecran','o='+o).catch(function(){})}catch(x){}
 if(o=='capter'){chaines();capEtat();capFichiers()}if(o=='reglages'){lireConfig();reseau();sauvs();sdbufLire();finspotLire();baseInfo()}if(o=='journal'){sd();journal()}}
function fmt(s){s=Math.max(0,Math.round(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')}
let etatOk=true,capOk=true;
function etat(){if(ong=='capter'||!etatOk||bzOcc)return;etatOk=false;fetch('/api/etat',{signal:AbortSignal.timeout(4000)}).then(r=>r.json()).then(e=>{E=e;
 const b=$('bandeau');b.style.setProperty('--c',COUL[e.etat]||'#3cff9c');$('mot').textContent=e.mot;{const h=$('heure-carte');if(h){h.textContent='Date de la carte : '+(e.heure||'?')+' · ce PC : '+new Date().toLocaleString('fr-FR',{dateStyle:'short',timeStyle:'medium'})}}$('sous').textContent=e.sous;
 $('actif').checked=!!e.actif;$('actif-txt').textContent=e.actif?'en marche':'arrêté';
 const bt=$('b-tester');bt.className=e.test==1?'ecoute':(e.test==4?'ecoute enreg':(e.test==2?'connue':''));
 bt.textContent=e.test==1?'J\'ÉCOUTE':(e.test==4?'ENREGISTRER':'TESTER');
 let t1='Une pub passe ?',t2='TESTER : la carte la reconnaît, ou l\'apprend si elle est nouvelle.',ann=false;
 if(e.test==1){t1='J\'écoute 3 s…';t2='Je cherche cette pub dans la bibliothèque.';ann=true}
 else if(e.test==4){t1='Pub inconnue : je l\'enregistre';t2=fmt(e.enreg_s)+' · ENREGISTRER à la fin de la pub';ann=true}
 else if(e.test==2&&e.test_appris){t1='Pub n°'+e.test_num+' apprise ✓';t2='ANNULER pour l\'oublier ('+Math.max(0,20-Math.round(e.test_age))+' s)';ann=true}
 else if(e.test==2){t1='Pub reconnue ✓';t2='n°'+e.test_num}
 else if(e.etat==4){t1='Pub reconnue';t2='Erreur ? « Ce n\'est pas une pub » corrige'}
 if(att){if(e.test!==att.test||Date.now()-att.t>3000)att=null;else{t1=att.q=='enregistrer'?"J'enregistre la pub…":(att.q=='annuler'?"J'annule…":'Je commence…');t2='un instant';ann=false}}
 $('t-titre').textContent=t1;$('t-sous').textContent=t2;$('b-annuler').style.display=ann?'':'none';
 $('b-paspub').style.display=e.etat==4&&!ann?'':'none';
 // coupure du son
 const c=$('coupure'),ce=e.coupe_etat,tvb=e.tv.baisse&&ce!=2;
 if(ce||tvb){c.style.display='';let ti,re='',f=0,col='#ff40c8';
  if(ce==1){ti='PUB CONFIRMÉE…';re='le son baisse';col='#ff8c3c'}
  else if(ce==2&&e.ec&&e.ec.cf>=2){ti='LE SON REMONTE (palier '+(e.ec.cf-1)+'/4) · fin de l\'écran de pub ?';re='volume d\'avant dans '+fmt(e.coupe_reste)+' sans nouvelle pub';f=e.coupe_total?e.coupe_reste/e.coupe_total:0;col='#ffd440'}
  else if(ce==2&&e.ec&&e.ec.cf){ti='SON BAISSÉ · j\'écoute la fibre';re='remonte dans '+fmt(e.coupe_reste)+' sans nouvelle pub';f=e.coupe_total?e.coupe_reste/e.coupe_total:0}
  else if(ce==2){ti='SON BAISSÉ';re='remonte dans '+fmt(e.coupe_reste);f=e.coupe_total?e.coupe_reste/e.coupe_total:0}
  else if(ce==3){ti='SON REMONTÉ';re='j\'écoute encore '+e.coupe_reste+' s';f=e.coupe_total?e.coupe_reste/e.coupe_total:0;col='#ffd440'}
  else{ti='SON BAISSÉ (essai)';re='onglet Télé → Rétablir';f=1}
  $('c-titre').textContent=ti;$('c-reste').textContent=re;c.style.borderColor=col;const bi=$('c-barre');bi.style.background=col;bi.style.width=(f*100)+'%';
  const T=e.tv;$('c-vol').textContent=T.action&&T.depart>=0?('Volume '+T.volume+' → '+T.cible+'…'):(T.connu?('Volume télé : '+T.volume):('Télé : '+T.nom))}
 else c.style.display='none';
 {const L=e.cps||[],K=$('cps');K.style.display=L.length?'':'none';
  if(L.length){const q=a=>a<60?a+' s':(a<3600?Math.round(a/60)+' min':Math.round(a/3600)+' h');
   $('cps-r').textContent='('+L.length+')';
   $('cps-l').innerHTML=L.map(x=>'<div class="ligne" style="margin:5px 0"><span class="grow" style="font-size:14px">il y a '+q(x.a)+' · '+(x.n?'pub <b>n°'+x.n+'</b>'+(x.c?' · '+x.c:'')+' · score '+x.s:'pub inconnue')+'</span>'+(x.n&&x.ok?'<button class="b-danger b-mini" onclick="cpsSuppr('+x.n+')">Fausse alerte</button>':(x.n?'<span class="petit">supprimée</span>':''))+'</div>').join('')}}
 if(e.msg_n!=vuMsg){vuMsg=e.msg_n;if(e.msg){$('msg').style.display='';$('msg').style.borderColor=e.msg_c;$('msg-t').textContent=e.msg;$('msg-d').textContent=e.msg_d;clearTimeout(window.tMsg);window.tMsg=setTimeout(function(){$('msg').style.display='none'},15000)}}
 if(window.vuTest!==e.test){if(e.test==1||e.test==4){$('msg').style.display='none';clearTimeout(window.tMsg)}window.vuTest=e.test}
 // tele
 const T=e.tv;volPuce(T.connu,T.volume,T.muet,T.action?T.cible:-1);
 const pc=T.etat==3?'#3cff9c':(T.etat==0?'#606c9c':(T.etat>=4?'#28e4ff':'#ff8c3c'));
 $('tv-past').style.background=pc;$('tv-past2').style.background=pc;
 $('tv-ligne').textContent='Télé '+(T.ip||'(IP à régler)')+' : '+T.nom+(T.connu?' · volume '+T.volume:'');
 $('tv-etat').textContent=T.nom+(T.appairee?' · appairée':' · pas appairée');
 $('tv-vol').textContent=T.connu?('Volume : '+T.volume+' / '+T.max+(T.muet?' (muet)':'')+(T.baisse?' · baissé par la carte (avant : '+T.origine+')':'')):'Volume : inconnu';
 if(T.msg_n!=vuTv){vuTv=T.msg_n;$('tv-msg').textContent=T.msg;$('tv-msg').style.color=T.msg_type==1?'#3cff9c':(T.msg_type==2?'#ff8c3c':'#9ca8d2')}
 if(document.activeElement!=$('tv-ip')&&!$('tv-ip').value)$('tv-ip').value=T.ip;
 $('code-zone').style.display=T.etat==6?'':'none';$('code-reste').textContent=T.reste+' s';
 $('b-app').disabled=T.etat>=4;$('b-app').textContent=T.appairee?'Réappairer':'Appairer';$('b-app-ann').style.display=T.etat>=4?'':'none';
 $('jr-on').checked=!!e.journal;$('jr-etat').textContent=e.journal?'gardé en mémoire':'coupé : seules les erreurs sont gardées';
 const p=$('plantage');if(e.plantages>0){p.style.display='';$('plantage-txt').textContent='Redémarrage anormal : '+e.demarrage+' ('+e.plantages+' depuis la mise sous tension). Le journal garde les lignes d\'avant.'}else p.style.display='none';
 ecMaj(e);
 $('pied').textContent='Détecteur de pub '+e.version+' · Arduino ESP32 '+e.arduino+' · ESP-IDF '+e.idf;
}).catch(()=>{}).finally(()=>{etatOk=true})}
// v0.9.63 : encadre « En ce moment » + compteurs en gros
function ecMaj(e){const x=e.ec;if(!x)return;const V='#3cff9c',O='#ff8c3c',R='#ff4e6a',G='#606c9c',M='#ff40c8';
 const L=(i,c,t)=>{$('ec-p'+i).style.background=c;$('ec-t'+i).textContent=t};
 {const t=$('ec-titre');if(t)t.textContent='EN CE MOMENT · '+(x.pr>=0&&PR[x.pr]?'préréglage : '+PR[x.pr].n:'réglage personnalisé');if(window.prVu!==x.pr){window.prVu=x.pr;prDessiner(x.pr)}}
 const dur=s=>s<60?s+' s':(s%60?Math.floor(s/60)+' min '+(s%60)+' s':(s/60)+' min');
 const niv=' · niveau '+Math.round(e.niveau)+' dB';
 if(!x.src){L(0,e.sature?R:(e.niveau<-65?O:V),'Micro · gain '+(x.g?'+'+x.gdb+' dB':'automatique (+'+x.gdb+' dB)')+(e.sature?' · SATURÉ':niv))}
 else if(e.fo==2)L(0,V,'Optique · son reçu'+niv);
 else if(e.fo==1)L(0,O,'Optique · silence : la télé n\'envoie rien');
 else if(e.fo==3)L(0,O,'Optique · test en cours');
 else L(0,R,'Optique · démarrage… le micro écoute encore');
 if(!e.actif)L(1,R,'Arrêtée : le son n\'est plus baissé');
 else L(1,e.nb_pubs?V:O,'En marche · sensibilité '+(['souple','normale','stricte'][x.se]||'?')+' · '+e.nb_pubs+' pub'+(e.nb_pubs>1?'s':''));
 const bas=x.bas?'son à '+x.bas:'son coupé (0)';
 L(2,e.actif?V:G,x.cm!=1?('Suivre la pub · '+bas+' pendant la pub'):(x.src?('Fibre · '+bas+' pendant la pub · volume d\'avant '+dur(x.cs)+' après la dernière pub (4 paliers)'):('Micro · '+bas+' pendant '+dur(x.cs)+' · puis réécoute '+x.re+' s')));
 if(!x.ce)L(3,G,'Aucune');
 else L(3,M,(['TOUT ENREGISTRER','DÉCOUPER','APPRENDRE'][x.ca])+' en cours · '+fmt(x.cd)+(x.ca?' · '+x.cg+(x.ca==2?' pub(s) apprise(s)':' pub(s) enregistrée(s)'):''));
 // v0.9.69 : memoire interne (vert : plus grand bloc >= 16 Ko ; orange : 8 a 16 Ko ; rouge : < 8 Ko ou < 15 Ko libres)
 if(x.ml!==undefined){const nv=(x.mb<8||x.ml<15)?2:(x.mb<16?1:0);
  L(4,[V,O,R][nv],['OK','chargée','⚠ SURCHARGE'][nv]+' · '+x.ml+' Ko libres · plus grand bloc '+x.mb+' Ko · mini '+x.mm+' Ko');
  const al=$('mem-alerte');al.style.display=nv==2?'':'none';
  if(nv==2)al.textContent='⚠ Attention : surcharge de la mémoire interne ('+x.ml+' Ko libres, plus grand bloc '+x.mb+' Ko). Fermez les onglets du portail inutiles ; si ça dure, redémarrez la carte.'}
 $('compteurs').innerHTML='<b>'+e.nb_pubs+'</b> pubs · <b>'+e.a_verifier+'</b> à vérifier · <b>'+(e.det||0)+'</b> détections · <b>'+e.coupures+'</b> coupures'+(e.sauve_echec?'<div style="color:var(--rouge);font-weight:700">ÉCHEC de la sauvegarde sur la carte SD</div>':'');
}
// v0.9.64 : spectre du son en spectrogramme defilant (comme l'ecran de la carte).
// 4 lectures par seconde au plus, seulement sur l'accueil visible, rien pendant une captation ou un TEST FO.
let spOcc=false,spPal=null;
function spCouleur(v){if(!spPal){spPal=[];const P=[[6,6,26],[40,20,120],[140,40,200],[255,64,200],[255,212,64],[255,255,255]];
 for(let i=0;i<256;i++){const t=i/255*(P.length-1),k=Math.min(P.length-2,Math.floor(t)),f=t-k,a=P[k],b=P[k+1];spPal.push('rgb('+Math.round(a[0]+(b[0]-a[0])*f)+','+Math.round(a[1]+(b[1]-a[1])*f)+','+Math.round(a[2]+(b[2]-a[2])*f)+')')}}return spPal[v]}
function spectre(){const c=$('sp');if(!c||spOcc||ong!='direct'||document.hidden)return;
 if(E&&((E.ec&&E.ec.ce)||E.fo==3)){$('sp-etat').textContent='en pause (captation ou test)';return}
 spOcc=true;fetch('/api/spectre',{signal:AbortSignal.timeout(2000)}).then(r=>r.json()).then(d=>{const g=c.getContext('2d'),W=c.width,H=c.height,n=d.b.length,L=4;
  g.drawImage(c,-L,0);const h=H/n;for(let i=0;i<n;i++){g.fillStyle=spCouleur(d.b[i]);g.fillRect(W-L,H-(i+1)*h,L,Math.ceil(h))}
  $('sp-etat').textContent=E&&E.ec&&E.ec.src?'entrée optique':'micro de la carte'}).catch(()=>{}).finally(()=>{spOcc=false})}
// v0.9.66 : preregages
let PR=[],prSel=-1;const PR_IC=['📡','⚡','🎤','⭐'];
function prResume(p){return (p.src?'optique':'micro')+' · '+(p.cm?'fenêtres':'suivre')+' · son '+p.bas+' · '+p.cs+' s'}
function prLire(){fetch('/api/prereg').then(r=>r.json()).then(d=>{PR=d.p||[];if(prSel<0)prSel=d.a>=0?d.a:3;prDessiner(d.a)}).catch(()=>{})}
function prDessiner(a){if(a===undefined)a=E&&E.ec?E.ec.pr:-1;
 const acc=$('pr-acc');if(acc)acc.innerHTML=PR.map((p,i)=>'<button class="prb'+(i==a?' on':'')+'" onclick="prAppl('+i+')"><span class="ic">'+PR_IC[i]+'</span>'+esc(p.n)+'<small>'+(i==a?'actif':esc(prResume(p)))+'</small></button>').join('');
 const c=$('pr-cases');if(c)c.innerHTML=PR.map((p,i)=>'<button class="b-mini'+(i==prSel?' b-ok':'')+'" onclick="prCase('+i+')">'+(i+1)+' · '+esc(p.n)+'</button>').join('');
 const n=$('pr-nom');if(n&&document.activeElement!=n&&PR[prSel])n.value=PR[prSel].n}
function prCase(i){prSel=i;const n=$('pr-nom');if(n&&PR[i])n.value=PR[i].n;prDessiner()}
function prAppl(i){post('/api/prereg','op=appliquer&i='+i).then(t=>{if(t!='OK'){toast(t);return}toast('Préréglage « '+PR[i].n+' » appliqué');prSel=i;setTimeout(()=>{prLire();lireConfig();etatOk=true;etat()},300)})}
function prEnr(){if(prSel<0)return;const nom=$('pr-nom').value.trim();if(!confirm('Enregistrer les réglages actuels dans la case '+(prSel+1)+(nom?' « '+nom+' »':'')+' ?'))return;
 post('/api/prereg','op=enregistrer&i='+prSel+'&nom='+encodeURIComponent(nom)).then(t=>{toast(t=='OK'?'Préréglage enregistré':t);prLire()})}
function prOrigine(){if(!confirm('Remettre les 4 préréglages d\'origine ? (la case 4 reprend vos réglages actuels)'))return;post('/api/prereg','op=origine').then(t=>{toast(t=='OK'?'Préréglages d\'origine remis':t);prLire()})}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function lireConfig(){fetch('/api/config').then(r=>r.json()).then(c=>{document.querySelectorAll('[data-c]').forEach(e=>{const v=c[e.dataset.c];if(v!==undefined)e.value=v});cfgLue=true})}
function envoyerConfig(){const o={};document.querySelectorAll('#s-reglages [data-c]').forEach(e=>{o[e.dataset.c]=e.dataset.t?e.value:parseInt(e.value)});
 fetch('/api/config',{method:'POST',body:JSON.stringify(o)}).then(r=>r.ok?r.json():r.text().then(t=>{throw t})).then(j=>{const fv=$('finspot').value;const p=(finspotV!==null&&fv!==finspotV)?finspotEnv(fv).then(()=>1):Promise.resolve(0);return p.then(n=>{toast((j.changes+n)+' réglage(s) changé(s)');lireConfig()})}).catch(t=>toast(t))}
function reseau(){fetch('/api/reseau').then(r=>r.json()).then(w=>{$('wifi-etat').textContent=w.mode==2?('Connectée à '+w.ssid+' · '+w.url+' · http://detecteur-pub.local'):('Point d\'accès '+w.ssid+' (mot de passe '+w.ap_mdp+')'+(w.wifi_regle?' · box '+w.wifi_regle+' injoignable':''));if(!$('w-ssid').value)$('w-ssid').value=w.wifi_regle})}
function wifi(){post('/api/wifi','ssid='+encodeURIComponent($('w-ssid').value)+'&mdp='+encodeURIComponent($('w-mdp').value)).then(toast)}
function cls(t){if(/^E \(/.test(t)||/ECHEC|chec|Erreur|impossible/.test(t))return 'e';if(/^W \(/.test(t))return 'w';if(/^\[TV\]/.test(t))return 't';if(!/^\[PUB\]/.test(t))return 's';return ''}
let jrOk=true;
function journal(){if(ong!='journal'||jrPause||!jrOk)return;jrOk=false;fetch('/api/journal?depuis='+jrDer,{signal:AbortSignal.timeout(6000)}).then(r=>r.json()).then(j=>{const z=$('jr');if(j.der<jrDer){z.innerHTML='';}
 const bas=z.scrollTop+z.clientHeight>=z.scrollHeight-20;j.lignes.forEach(l=>{const d=document.createElement('div');d.className=cls(l.t);d.textContent=l.t;z.appendChild(d)});
 while(z.childNodes.length>400)z.removeChild(z.firstChild);jrDer=j.der;if(bas)z.scrollTop=z.scrollHeight}).catch(()=>{}).finally(()=>{jrOk=true})}
function sd(){fetch('/api/sd',{signal:AbortSignal.timeout(8000)}).then(r=>r.ok?r.json():null).then(s=>{if(!s){$('sd-info').textContent='Pas de carte SD';return}
 $('sd-info').textContent=(s.libre/1e9).toFixed(1)+' Go libres sur '+(s.total/1e9).toFixed(1)+' Go';
 const f=s.fichiers.filter(x=>!/^\/pub\/(audio|tv)\//.test(x.nom));$('sd').innerHTML=f.map(x=>'<div class="ligne" style="padding:4px 0"><a class="grow petit" href="/api/sd/fichier?f='+encodeURIComponent(x.nom)+'">'+esc(x.nom)+'</a><span class="tres">'+(x.t?new Date(x.t*1000).toLocaleString('fr-FR',{dateStyle:'short',timeStyle:'short'})+' · ':'')+(x.nb!==undefined?x.nb+' pubs · ':'')+(x.taille/1024).toFixed(0)+' Ko</span></div>').join('')}).catch(()=>{}).catch(()=>{})}
function capEtat(){if((ong!='capter'&&ong!='reglages')||!capOk)return;capOk=false;fetch('/api/capter',{signal:AbortSignal.timeout(4000)}).then(r=>r.json()).then(c=>{
 const ap=!!c.ap,nm=ap?'APPRENDRE':'DÉCOUPER';
 volPuce(c.vol>=0,c.vol,c.muet,c.volcible);
 const sel=$('cap-ch');if(chListe&&document.activeElement!=sel)sel.value=c.chsel;sel.disabled=c.cap!=0;
 const b=$('b-cap');$('b-cap-ann').style.display=(c.cap==1||c.cap==2)?'':'none';
 $('b-cap-ann').textContent=(c.auto&&ap)?'ANNULER (les pubs apprises restent)':'ANNULER la captation (supprime tout)';
 if(c.cap==2){b.className='b-danger';b.textContent='ARRÊTER '+fmt(c.s)+' / '+fmt(c.max);b.disabled=false}
 else if(c.cap==1||c.cap==3){b.className='b-mini';b.textContent=c.cap==1?'un instant…':"j'arrête…";b.disabled=true;setTimeout(capEtat,300)}
 else{b.className='b-ok';b.textContent='⏺ TOUT ENREGISTRER';b.disabled=c.fo!=0}
 $('blk-auto').style.display=$('blk-appr').style.display=$('cap-aide').style.display=c.cap==0?'':'none';
 {const st=$('cap-stats'),vis=c.cap!=0?!!c.auto:!!c.ab;st.style.display=vis?'':'none';
  if(vis){const live=c.cap!=0,n=(c.ag+c.ac+c.ak+c.aj),moy=c.ag>0?c.at/c.ag:0;
   $('cs-titre').textContent=live?'Statistiques '+nm+' (en direct)':'Bilan de la dernière captation '+nm;
   const L=(t,v,col)=>'<div><span style="color:var('+col+');font-weight:700">'+v+'</span> '+t+'</div>';
   const kl=(c.kl||[]),T=(t)=>'<div style="grid-column:1/3">'+t+'</div>';
   $('cs-cpt').innerHTML=T('Durée captée : <b>'+fmt(c.s)+'</b>')
    +L((ap?'apprise(s)':'enregistrée(s)')+' · '+fmt(c.at)+(c.ag>0?' (moy. '+fmt(moy)+')':''),c.ag,'--vert')
    +(ap&&(c.ag||c.su)?T('<span class="petit">dont <b style="color:var(--vert)">'+(c.su||0)+'</b> sûre(s) ou revue(s) : OK tout de suite · le reste dans « Pubs apprises, à vérifier »</span>'):'')
    +L('connue(s) entendue(s) · '+fmt(c.cs||0),c.ac,'--cyan')
    +L('trop courte(s)',c.ak,'--jaune')+L('jetée(s)',c.aj,'--rouge')
    +(kl.length?T('<span class="petit">Pubs connues : n°'+kl.join(', n°')+'</span>'):'')
    +(c.sc?T('<span style="color:var(--jaune)">'+c.sc+' coupée(s) sans vrai silence : fichiers « _sans_silence », à vérifier</span>'):'')
    +((c.ns||0)>=1?T('<span style="color:var(--rouge)">Non traité : <b>'+fmt(c.ns)+'</b> (pas de silence pour couper, morceau trop long ou jeté)</span>'):'')
    +(c.ga?T('<span style="color:var(--jaune)">⚠ Captation n°'+c.num+' gardée : à traiter dans Python (Fichiers de captation → Enregistrements entiers)</span>'):'')
    +(live?T('Morceau en cours : <b>'+fmt(c.as)+'</b>'):'');
   const col={1:'--vert',2:'--jaune',3:'--cyan',4:'--rouge'},ico={1:'✔',2:'✂',3:'≡',4:'✖'};
   $('cs-ev').innerHTML=(c.ev&&c.ev.length?c.ev:[]).map(e=>'<div style="color:var('+(col[e.t]||'--texte')+')">'+(ico[e.t]||'')+' '+e.x.replace(/&/g,'&amp;').replace(/</g,'&lt;')+(e.n?' <span class="petit">'+e.n.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</span>':'')+'</div>').join('')||'<span class="petit">En attente de la première pub…</span>';}}
 $('cap-info').textContent=c.cap==2?(c.auto?(ap?'APPRENDRE : '+c.ag+' pub(s) apprise(s) dans la bibliothèque, ':'DÉCOUPER n°'+c.num+' : '+c.ag+' pub(s) inconnue(s) enregistrée(s), ')+c.ac+' connue(s) ignorée(s)'+(c.as>0?' · morceau en cours '+fmt(c.as):'')+' · le son de la télé n\'est pas baissé.':'Captation n°'+c.num+' en cours : le son de la télé n\'est pas baissé.'):(c.capfins?'Dernière : n°'+c.num+' · '+c.captxt:'');
 const f=$('b-fo');f.disabled=c.fo!=0||c.cap!=0;f.textContent=c.fo==1?'J\'écoute la fibre… '+Math.round(c.fos)+' / 10 s · niveau '+Math.round(c.fon*100)+' %':(c.fo==2?'j\'écris sur la SD…':'📡 TEST FO (10 s)');
 $('fo-info').textContent=c.fo==0&&c.fofins?'Dernier test : '+c.fotxt:'';
 if(c.capfins!=capVu||c.fofins!=foVu){const n=capVu>=0;capVu=c.capfins;foVu=c.fofins;if(n&&ong=='capter')capFichiers()}
}).catch(()=>{}).finally(()=>{capOk=true})}
let capVu=-1,foVu=-1;
let chListe=false;
function capCh(v){post('/api/capter','a=chaine&v='+v).catch(()=>{})}
function chaines(){if(chListe)return;fetch('/api/chaines').then(r=>r.json()).then(l=>{chListe=true;$('cap-ch').innerHTML=l.map((n,i)=>'<option value="'+i+'">'+esc(n)+'</option>').join('');capEtat()}).catch(()=>{})}
let finspotV=null;function finspotLire(){fetch('/api/finspot').then(r=>r.json()).then(s=>{$('finspot').value=s.auto;finspotV=String(s.auto)}).catch(()=>{})}
function finspotEnv(v){return post('/api/finspot','v='+v).then(()=>{finspotV=String(v)})}
function plantageEff(){post('/api/plantage_effacer').then(()=>{$('plantage').style.display='none';toast('Message effacé')})}

function mo(o){return (o/1048576).toFixed(1).replace('.',',')+' Mo'}
let bzAbort=null,bzXhr=null;
function tps(s){s=Math.max(0,Math.round(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')}
function bzTemps(f,t0){const s=(Date.now()-t0)/1000;return ' · écoulé '+tps(s)+(f>0.03&&s>2?' · reste ≈ '+tps(s*(1-f)/f):'')}
function bzFin(){bzOcc=false;bzAbort=null;bzXhr=null;$('bz-stop').style.display='none';$('bz-dl').disabled=false}
function baseStop(){if(!confirm('Arrêter le transfert en cours ?'))return;if(bzAbort)bzAbort.abort();if(bzXhr)bzXhr.abort()}
function bzBarre(f,txt){$('bz-barre').style.display='';$('bz-i').style.width=(f*100).toFixed(1)+'%';$('bz-txt').textContent=txt}
async function baseDebit(){const o=$('bz-deb');if(bzOcc){o.textContent='Un transfert est en cours : arrête-le (bouton rouge) ou attends sa fin, puis relance la mesure.';return}bzOcc=true;
 try{o.textContent='Mesure réseau (2,5 Mo, sans la SD)…';const t0=Date.now();const r=await fetch('/api/debit?m=reseau');const rssi=r.headers.get('X-RSSI'),ps=r.headers.get('X-PS'),mem=r.headers.get('X-MEM'),phy=r.headers.get('X-PHY');const rd=r.body.getReader();let n=0;
  for(;;){const {done,value}=await rd.read();if(done)break;n+=value.length;const s0=(Date.now()-t0)/1000;o.textContent='Mesure réseau : '+mo(n)+' / 2,5 Mo · '+(s0>0.3?mo(n/s0)+'/s':'')+' · écoulé '+s0.toFixed(0)+' s'}
  const s=(Date.now()-t0)/1000;const rs=n/s;
  const va=await (await fetch('/api/debit?m=variantes')).json();const nm=['A blocs 16 Ko','B avec Nagle','C blocs 1,4 Ko','D send direct','E reste de la carte endormi'];
  const dv=va.map((ms,i)=>nm[i]+' : '+(ms>0?mo(524288/(ms/1000))+'/s':'?')).join(' · ');
  o.textContent='Réseau seul : '+mo(rs)+'/s · signal '+rssi+' dBm · veille Wi-Fi : '+(ps=='0'?'aucune':'active ('+ps+')')+' · variantes : '+dv+' · mémoire interne libre/bloc max/minimum (Ko) : '+mem+' · '+phy+'. Mesure SD…';
  const j=await (await fetch('/api/debit?m=sd')).json();const ss=j.octets/(j.ms/1000);
  o.textContent='Mesure PC → carte (1 Mo)…';const fd=new FormData();fd.append('x',new Blob([new Uint8Array(1048576)]),'x.bin');const tu=Date.now();
  const ju=await fetch('/api/debit_up',{method:'POST',body:fd}).then(r=>r.json());const up=ju.octets/((Date.now()-tu)/1000);
  o.textContent='Réseau seul : '+mo(rs)+'/s · SD seule : '+mo(ss)+'/s · signal '+rssi+' dBm · veille Wi-Fi : '+(ps=='0'?'aucune':'active ('+ps+')')+' · PC → carte : '+mo(up)+'/s · variantes : '+dv+' · mémoire interne libre/bloc max/minimum (Ko) : '+mem+' · '+phy}catch(e){o.textContent='Mesure interrompue'}finally{bzOcc=false}}
function baseInfo(){fetch('/api/base_info').then(r=>r.ok?r.json():null).then(s=>{if(!s){$('bz-info').textContent='';return}
 $('bz-resume').textContent=s.pubs+' pubs · '+s.nv+' à vérifier · '+(s.det||0)+' détections · dernière base reçue du PC : '+(s.synchro?new Date(s.synchro*1000).toLocaleString('fr-FR',{dateStyle:'short',timeStyle:'short'}):'jamais');
 $('bz-info').textContent='Volume à transférer : '+mo(s.octets)+' · '+s.fichiers+' fichiers · '+s.pubs+' pubs'+(s.a_jour?'':' · (modifiée à l\'instant : attends quelques secondes)')+(s.ms_tot?' · dernier transfert : '+(s.ms_tot/1000).toFixed(0)+' s dont SD '+(s.ms_sd/1000).toFixed(0)+' s, réseau '+(s.ms_res/1000).toFixed(0)+' s':'')}).catch(()=>{})}
async function baseTelecharger(){
 if(bzOcc)return;bzOcc=true;$('bz-dl').disabled=true;$('bz-conf').style.display='none';bzAbort=new AbortController();$('bz-stop').style.display='';
 try{const r=await fetch('/api/base_zip',{signal:bzAbort.signal});if(!r.ok){toast(await r.text());return}
  const tot=parseInt(r.headers.get('Content-Length')||'0');const rd=r.body.getReader();const ch=[];let recu=0;const t0=Date.now();
  bzBarre(0,'Téléchargement carte → PC : 0 / '+mo(tot));
  for(;;){const {done,value}=await rd.read();if(done)break;ch.push(value);recu+=value.length;const s=(Date.now()-t0)/1000;
   bzBarre(tot?recu/tot:0,'Téléchargement carte → PC : '+mo(recu)+' / '+mo(tot)+' · '+(tot?Math.round(recu*100/tot):0)+' % · '+(s>0.5?mo(recu/s)+'/s':'')+bzTemps(tot?recu/tot:0,t0))}
  if(tot&&recu!=tot){toast('Téléchargement incomplet : recommence');return}
  const d=new Date();const nom='base_pub_'+d.getFullYear()+String(d.getMonth()+1).padStart(2,'0')+String(d.getDate()).padStart(2,'0')+'_'+String(d.getHours()).padStart(2,'0')+String(d.getMinutes()).padStart(2,'0')+'.zip';
  const a=document.createElement('a');a.href=URL.createObjectURL(new Blob(ch,{type:'application/zip'}));a.download=nom;document.body.appendChild(a);a.click();a.remove();
  bzBarre(1,'Téléchargement terminé : '+mo(recu)+' reçus · fichier '+nom+' (dans Téléchargements)');
 }catch(e){const ar=bzAbort&&bzAbort.signal.aborted;bzBarre(0,ar?'Transfert annulé : rien n\'a été enregistré.':'Téléchargement interrompu');toast(ar?'Transfert annulé':'Téléchargement interrompu')}finally{bzFin()}}
function baseEnvoyer(inp){const f=inp.files[0];inp.value='';if(!f||bzOcc)return;bzOcc=true;$('bz-conf').style.display='none';
 const fd=new FormData();fd.append('zip',f,f.name);const x=new XMLHttpRequest();const t0=Date.now();bzXhr=x;$('bz-stop').style.display='';$('bz-dl').disabled=true;
 x.open('POST','/api/base_envoi');
 x.upload.onprogress=e=>{if(e.lengthComputable){const s=(Date.now()-t0)/1000;bzBarre(e.loaded/e.total,'Envoi PC → carte : '+mo(e.loaded)+' / '+mo(e.total)+' · '+Math.round(e.loaded*100/e.total)+' % · '+(s>0.5?mo(e.loaded/s)+'/s':'')+bzTemps(e.loaded/e.total,t0))}};
 x.upload.onload=()=>{$('bz-txt').textContent='Vérification sur la carte…'};
 x.onload=()=>{bzFin();if(x.status==200){const j=JSON.parse(x.responseText);bzBarre(1,'Envoi terminé, base reçue et vérifiée par la carte : '+j.fichiers+' fichiers · '+mo(j.octets));
   $('bz-conf-txt').textContent='La base corrigée contient '+j.pubs_nouvelle+' pubs ; la carte en a '+j.pubs_actuelle+'. La remplacer ? (la carte redémarre)';$('bz-conf').style.display=''}
  else{bzBarre(0,'');$('bz-txt').textContent=x.responseText;toast(x.responseText)}};
 x.onerror=()=>{bzFin();$('bz-txt').textContent='Envoi interrompu';toast('Envoi interrompu')};
 x.onabort=()=>{bzFin();bzBarre(0,'Envoi annulé : la base de la carte n\'a pas été touchée.');toast('Envoi annulé')};
 x.send(fd)}
function baseAppliquer(){if(!confirm('Remplacer toute la base de la carte par la base corrigée ? L\'ancienne sera gardée dans /pub/ancienne_base/.'))return;
 post('/api/base_appliquer').then(t=>{$('bz-conf').style.display='none';$('bz-txt').textContent=t;toast(t);setTimeout(()=>location.reload(),25000)})}
function baseAnnuler(){post('/api/base_appliquer','a=annuler').then(t=>{$('bz-conf').style.display='none';$('bz-txt').textContent=t})}
function sdbufLire(){fetch('/api/sdbuf').then(r=>r.json()).then(s=>{$('sdbuf').value=s.choisi;$('sdbuf-info').textContent='Actif maintenant : '+(s.actif?s.actif+' Ko':'pas encore utilisé')+' · choisi : '+s.choisi+' Ko'+(s.actif&&s.actif!=s.choisi?' (sera appliqué au prochain démarrage)':'')}).catch(()=>{})}
function sdbufEnv(v){post('/api/sdbuf','v='+v).then(t=>{toast(t);sdbufLire()})}
function volPuce(connu,vol,muet,cible){const p=$('volchip');p.style.display='';p.textContent='🔊 '+(connu?(muet?'muet':vol):'?')+(cible>=0&&connu&&cible!=vol?' → '+cible:'')}
function redemarrer(){if(!confirm('Redémarrer la carte maintenant ?'))return;
 post('/api/redemarrer').then(t=>{$('rb-info').textContent=t;if(!/^Redémarrage/.test(t))return;let bas=false,n=0;
  const tm=setInterval(()=>{n++;fetch('/api/etat',{signal:AbortSignal.timeout(2500)}).then(r=>r.ok?r.json():Promise.reject()).then(()=>{if(bas){clearInterval(tm);location.reload()}else if(n>3){$('rb-info').textContent='En attente du redémarrage…'}}).catch(()=>{bas=true;$('rb-info').textContent='La carte redémarre…'});if(n>90){clearInterval(tm);$('rb-info').textContent='Pas de réponse : recharge la page ou débranche / rebranche la carte.'}},1500)})}
function capAnnuler(){if(!confirm(/apprises/.test($('b-cap-ann').textContent)?'Annuler ? Les fichiers sont supprimés, les pubs déjà apprises restent dans la bibliothèque.':'Supprimer cette captation ?'))return;post('/api/capter','a=annuler').then(toast);[150,400,800,1500].forEach(t=>setTimeout(capEtat,t))}
function cpsSuppr(n){if(confirm('Supprimer la pub n°'+n+' de la bibliothèque (fausse alerte) ?'))post('/api/cmd','c=supprimer&n='+n).then(t=>{toast(t);etatOk=true;etat()})}
function capter(){const b=$('b-cap'),arret=/ARRÊTER/.test(b.textContent);
 b.className='b-mini';b.disabled=true;b.textContent=arret?"j'arrête…":'un instant…';
 post('/api/capter','a='+(arret?'stop':'start')).then(toast);
 [150,400,800,1500].forEach(t=>setTimeout(capEtat,t))}
function capterAuto(){const b=$('b-cap');b.className='b-mini';b.disabled=true;b.textContent='un instant…';$('blk-auto').style.display=$('blk-appr').style.display=$('cap-aide').style.display='none';
 post('/api/capter','a=auto').then(toast);
 [150,400,800,1500].forEach(t=>setTimeout(capEtat,t))}
function capterAppr(){const b=$('b-cap');b.className='b-mini';b.disabled=true;b.textContent='un instant…';$('blk-auto').style.display=$('blk-appr').style.display=$('cap-aide').style.display='none';
 post('/api/capter','a=apprendre').then(toast);
 [150,400,800,1500].forEach(t=>setTimeout(capEtat,t))}
function testFo(){post('/api/fo').then(toast);setTimeout(capEtat,300)}
// v0.9.58 : lecteur dans la page (un seul a la fois) ; ▶ / ■
let lecSrc='';
function jouer(f){const a=$('lecteur');if(lecSrc==f&&!a.paused){a.pause();lecSrc='';majLec();return}
 lecSrc=f;a.src='/api/sd/fichier?f='+encodeURIComponent(f);a.play().catch(()=>{toast('Lecture impossible');lecSrc='';majLec()});majLec()}
function majLec(){document.querySelectorAll('button[data-j]').forEach(b=>{b.textContent=(b.dataset.j==lecSrc)?'■':'▶'})}
document.addEventListener('DOMContentLoaded',()=>{const a=$('lecteur');if(a)a.addEventListener('ended',()=>{lecSrc='';majLec()})});
const bJ=f=>'<button class="b-mini" title="Écouter" data-j="'+esc(f)+'" onclick="jouer(this.dataset.j)">'+(f==lecSrc?'■':'▶')+'</button>';
// v0.9.58 : pubs apprises « à vérifier » (APPRENDRE, bouton) : valider ou supprimer, sans PC
const AV_O={0:'bouton',1:'import',2:'APPRENDRE',3:'réseau'},AV_R={1:'pas de silence d\'un côté',2:'durée inhabituelle',3:'ressemble à une pub connue',4:'trop courte',5:'apprise pendant une coupure, entre deux pubs'};
function avRendre(d){const l=(d&&d.l)||[],t='Pubs apprises, à vérifier';if(!l.length){$('av-liste').innerHTML='';return}
 const ouvert=capOuverts.has(t),dt=x=>x?new Date(x*1000).toLocaleString('fr-FR',{dateStyle:'short',timeStyle:'short'}):'';
 l.sort((a,b)=>b.t-a.t);
 $('av-liste').innerHTML='<details data-t="'+esc(t)+'"'+(ouvert?' open':'')+' ontoggle="capPli(this)" style="margin:6px 0"><summary style="cursor:pointer;font-weight:700;color:var(--jaune)">'+t+' · '+l.length+' pub(s)</summary>'
  +'<div class="petit" style="margin:6px 0">Écoute, puis ✓ si c\'est bien une pub (elle passe « OK »), ✕ sinon (retirée de la bibliothèque).</div>'
  +'<div class="ligne wrap" style="gap:8px;margin:8px 0"><button class="b-mini b-ok" onclick="avOp(\'ok_tous\','+l.length+')">✓ Tout valider</button><button class="b-mini b-danger" onclick="avOp(\'suppr_tous\','+l.length+')">✕ Tout supprimer</button></div>'
  +l.map(x=>'<div class="ligne" style="padding:4px 0;gap:6px">'+(x.a?bJ('/pub/audio/'+x.n+'.wav'):'<span class="tres">—</span>')
   +'<span class="grow"><b>n°'+x.n+'</b> · '+fmt(x.d)+(x.ch?' · '+esc(x.ch):'')+' · '+(AV_O[x.o]||'')+(x.t?' · '+dt(x.t):'')
   +(AV_R[x.r]?'<br><span class="tres" style="color:var(--jaune)">'+AV_R[x.r]+'</span>':'')+'</span>'
   +'<button class="b-mini b-ok" title="C\'est une pub" onclick="avOp(\'ok\',0,'+x.id+')">✓</button><button class="b-mini b-danger" title="Pas une pub" onclick="avOp(\'suppr\',0,'+x.id+')">✕</button></div>').join('')+'</details>'}
function avOp(op,n,id){if(op=='suppr_tous'&&!confirm('Supprimer les '+n+' pub(s) à vérifier de la bibliothèque ?'))return;
 if(op=='ok_tous'&&!confirm('Valider les '+n+' pub(s) à vérifier ?'))return;
 post('/api/averifier','op='+op+(id!==undefined?'&id='+id:'')).then(t=>{if(t!='OK')toast(t);setTimeout(()=>{capFichiers();baseInfo()},400)})}
// v0.9.58 : fichiers AUTO : « Dans la base ? » calcule par la carte, ✓ = ajouter la pub (OK)
const FE={1:'en attente…',2:'analyse…',3:'nouvelle',7:'erreur de lecture',8:'bibliothèque pleine',9:'trop silencieux'};
function feTxt(e){if(!e)return'';if(e.e==4)return'<span style="color:var(--jaune)">? ressemble à n°'+e.u+' ('+e.p+' %)</span>';
 if(e.e==5)return'<span style="color:var(--vert)">✓ déjà dans la base : n°'+e.u+'</span>';if(e.e==6)return'<span style="color:var(--vert)">ajoutée : pub n°'+e.u+'</span>';
 return'<span class="tres">'+(FE[e.e]||'')+'</span>'}
function fiOp(op,f){const msg={comparer:'Comparaison lancée (environ 1 s par fichier).',nouvelles:'Ajout des nouvelles lancé.',suppr_connus:'Fichiers déjà dans la base effacés.'};
 if(op=='suppr_connus'&&!confirm('Effacer les fichiers déjà dans la base ? (la base n\'est pas touchée)'))return;
 post('/api/fi','op='+op+(f?'&f='+encodeURIComponent(f):'')).then(t=>{if(msg[op])toast(msg[op]+(op=='suppr_connus'?' ('+t+')':''));else if(t=='0')toast('Déjà en cours');setTimeout(capFichiers,300)})}
let fiSuivi=null;
function capFichiers(){Promise.all([fetch('/api/sd?d=/pub/captations').then(r=>r.ok?r.json():null),fetch('/api/fi').then(r=>r.ok?r.json():null).catch(()=>null),fetch('/api/averifier').then(r=>r.ok?r.json():null).catch(()=>null)]).then(([s,fi,av])=>{
 avRendre(av);
 if(!s){$('cap-fichiers').textContent='Pas de carte SD';return}
 // v0.9.54 : un resume par groupe, replie ; ⬇ = tout le groupe en ZIP (pour gestion_pubs.py), Vider = tout supprimer
 const P='/pub/captations/',G=[['auto','Pubs découpées (DÉCOUPER) : ▶ écouter, ✓ garder, ✕ jeter',x=>x.indexOf(P+'auto/')==0],['cap','Enregistrements entiers (TOUT ENREGISTRER)',x=>/^\/pub\/captations\/(cap_\d+\.wav|index\.csv)$/.test(x)],['fo','Test de l\'entrée optique',x=>/^\/pub\/captations\/fo_test/.test(x)],['','Autres fichiers',x=>true]];
 const fe={};((fi&&fi.l)||[]).forEach(x=>fe[x.n]=x);
 const f=s.fichiers.filter(x=>x.nom.indexOf(P)==0),pris=new Set();
 const h=G.map(([g,t,test])=>{const l=f.filter(x=>!pris.has(x.nom)&&test(x.nom));l.forEach(x=>pris.add(x.nom));if(!l.length)return'';
  const tot=l.reduce((a,x)=>a+x.taille,0),ouvert=capOuverts.has(t),wav=x=>/\.wav$/i.test(x.nom);
  const lignes=l.sort((a,b)=>b.nom.localeCompare(a.nom)).map(x=>{const court=x.nom.replace(P,''),base=x.nom.replace(P+'auto/',''),e=g=='auto'?fe[base]:null;
   return '<div class="ligne" style="padding:4px 0;gap:6px">'+(wav(x)?bJ(x.nom):'')+'<span class="grow"><a href="/api/sd/fichier?f='+encodeURIComponent(x.nom)+'">'+esc(court)+'</a>'+(e?'<br>'+feTxt(e):'')+'</span><span class="tres">'+(x.taille/1048576).toFixed(1)+' Mo</span>'
    +(g=='auto'&&wav(x)?'<button class="b-mini b-ok" title="Ajouter comme pub (OK)" data-f="'+esc(base)+'" onclick="fiOp(\'valider\',this.dataset.f)">✓</button>':'')
    +'<button class="b-mini b-danger" data-f="'+esc(x.nom)+'" onclick="supprFichier(this.dataset.f)">✕</button></div>'}).join('');
  const nbConnus=g=='auto'?l.filter(x=>{const e=fe[x.nom.replace(P+'auto/','')];return e&&e.e==5}).length:0,nbNouv=g=='auto'?l.filter(x=>{const e=fe[x.nom.replace(P+'auto/','')];return e&&e.e==3}).length:0;
  return '<details data-t="'+esc(t)+'"'+(ouvert?' open':'')+' ontoggle="capPli(this)" style="margin:6px 0"><summary style="cursor:pointer;font-weight:700">'+esc(t)+' · '+l.length+' fichier(s) · '+mo(tot)+'</summary>'
   +(g?'<div class="ligne wrap" style="gap:8px;margin:8px 0"><a class="b-mini" style="background:var(--carte2);border-radius:10px;padding:7px 10px;font-weight:700;text-decoration:none;color:var(--txt)" href="/api/cap/zip?g='+g+'">⬇ Tout télécharger (ZIP)</a><button class="b-mini b-danger" onclick="capVider(\''+g+'\',\''+esc(t).replace(/'/g,"\\'")+'\','+l.length+')">Vider</button></div>':'')
   +(g=='auto'?'<div class="ligne wrap" style="gap:8px;margin:8px 0"><button class="b-mini" onclick="fiOp(\'comparer\')">🔎 Dans la base ?</button>'
     +(nbNouv?'<button class="b-mini b-ok" onclick="fiOp(\'nouvelles\')">✓ Valider les '+nbNouv+' nouvelle(s)</button>':'')
     +(nbConnus?'<button class="b-mini b-danger" onclick="fiOp(\'suppr_connus\')">✕ Effacer les '+nbConnus+' déjà dans la base</button>':'')+'</div>'
     +(fi&&fi.file?'<div class="petit" style="color:var(--jaune);margin:4px 0">'+(fi.cap?'En attente de la fin de la captation… ':'Traitement : '+fi.file+' fichier(s) en cours (la détection prend 1 à 2 s de retard par fichier)')+'</div>':''):'')
   +lignes+'</details>'}).join('');
 $('cap-fichiers').innerHTML=h||'Aucune captation.';
 // pendant un traitement : la liste se rafraichit seule
 clearTimeout(fiSuivi);if(fi&&fi.file&&ong=='capter')fiSuivi=setTimeout(()=>{capFichiers();baseInfo()},1500)})}
const capOuverts=new Set();
function capPli(d){if(d.open)capOuverts.add(d.dataset.t);else capOuverts.delete(d.dataset.t)}
function capVider(g,t,n){if(!confirm('Vider « '+t+' » ? Les '+n+' fichier(s) seront supprimés de la carte SD.'))return;post('/api/cap/vider','g='+g).then(t=>{toast(t);capFichiers()})}
function supprFichier(n){if(confirm('Supprimer '+n+' ?'))post('/api/sd/suppr','f='+encodeURIComponent(n)).then(t=>{toast(t);capFichiers();sauvs()})}
// v0.9.70 : la carte ecrit sans bloquer la page ; on demande ou elle en est chaque seconde
function sauvegarder(){const m=$('sauv-msg');m.textContent='Sauvegarde en cours…';toast('Sauvegarde en cours…');let t0=Date.now();
 const fin=t=>{m.textContent=t;toast(t);sauvs()};
 const suivre=()=>post('/api/sauvegarder','op=etat').then(t=>{if(t=='EN_COURS'){m.textContent='Sauvegarde en cours… '+Math.round((Date.now()-t0)/1000)+' s';setTimeout(suivre,1000)}else fin(t)}).catch(()=>setTimeout(suivre,2000));
 post('/api/sauvegarder').then(t=>{if(t=='EN_COURS')setTimeout(suivre,1000);else fin(t)}).catch(()=>fin('Sauvegarde : la carte ne répond pas'))}
function sauvs(){fetch('/api/sd?d=/pub/sauvegardes').then(r=>r.ok?r.json():null).then(s=>{if(!s)return;
 const f=s.fichiers.filter(x=>x.nom.indexOf('/pub/sauvegardes/')==0);
 $('sauvs').innerHTML=f.length?f.map(x=>{const n=x.nom.replace('/pub/sauvegardes/','');return '<div class="ligne wrap" style="padding:4px 0"><a class="grow petit" href="/api/sd/fichier?f='+encodeURIComponent(x.nom)+'">'+esc(n)+'</a><span class="tres">'+(x.taille/1024).toFixed(0)+' Ko</span><button class="b-mini" data-n="'+esc(n)+'" onclick="restaurer(this.dataset.n)">Restaurer</button><button class="b-mini b-danger" data-f="'+esc(x.nom)+'" onclick="supprFichier(this.dataset.f)">✕</button></div>'}).join(''):'<div class="petit">Aucune sauvegarde.</div>'})}
function restaurer(n){if(confirm('Ajouter les pubs de '+n+' à la bibliothèque ?')){toast('Restauration…');post('/api/restaurer','nom='+encodeURIComponent(n)).then(toast)}}
function effacer(){if(prompt('Tape EFFACER pour supprimer toutes les pubs (une copie de sécurité est faite avant)')=='EFFACER'){toast('Effacement…');post('/api/effacer','confirme=EFFACER').then(t=>{toast(t);sauvs()})}}
function importer(i){const f=i.files[0];if(!f)return;const d=new FormData();d.append('f',f,'import.pub');toast('Envoi…');fetch('/api/import',{method:'POST',body:d}).then(r=>r.text()).then(t=>{toast(t);i.value=''})}
prLire();etat();setInterval(etat,1000);setInterval(spectre,250);setInterval(capEtat,1000);setInterval(journal,2000);
document.addEventListener('click',function(e){var b=e.target.closest&&e.target.closest('button');if(!b||b.disabled)return;b.classList.add('pris');setTimeout(function(){b.classList.remove('pris')},350)},true);
</script></body></html>)PAGE";
