# -*- coding: utf-8 -*-
"""
gestion_pubs.py - Detecteur de pub : gerer la bibliotheque des pubs sur PC.

La carte SD du detecteur (carte ES3C28P) est lue sur le PC :
  /pub/base.bin    les empreintes (format "PUB1" de base.cpp)
  /pub/infos.bin   les fiches : nom, chaine, a verifier... (format "INF2")
  /pub/audio/N.wav le son de la pub n°N (8 kHz, mono, 16 bits)

Ce qu'on peut faire : ecouter chaque pub (volume remonte automatiquement),
la marquer OK (plus "a verifier"), la renommer, choisir sa chaine, la
supprimer, ne garder qu'un passage (decoupe des empreintes ET du son).
Rien n'est ecrit tant qu'on ne clique pas sur "Enregistrer sur la carte" ;
avant d'ecrire, l'ancienne bibliotheque est copiee dans /pub/sauvegardes_pc/.

IMPORTANT : eteindre la carte avant de retirer la carte SD, et ne la remettre
qu'apres l'enregistrement.

v0.9.56 : ecran d'accueil a deux modes, gros boutons, un seul menu « ⋯ Plus ».
  MODE 1 carte SD dans le PC : « Ouvrir la carte SD » (trouvee toute seule ; ancien format -> mise a niveau
          proposee, copie complete ZIP avant), Verifier, Captations (celles de la carte), Enregistrer, Ejecter.
  MODE 2 portail : « Verifier les pubs » / « Ajouter des pubs depuis des captations » (le dernier .zip des
          Telechargements est trouve tout seul), puis « Terminer : preparer l'envoi ».
  Chiffres identiques a la carte : pubs · a verifier · detections (somme des passages reconnus).

FAÇON DE TRAVAILLER (v0.9.54) : sans retirer la carte SD, tout passe par le portail web.
  Gérer les pubs  : portail « Carte → PC » (ZIP)  →  ici « Ouvrir la base (ZIP) », revue, Ctrl+S
                    →  portail « PC → carte » avec le fichier base_corrigee.zip préparé ici.
  Captations      : portail, Fichiers de captation, « ⬇ Tout télécharger (ZIP) »  →  ici « Captations »
                    (la base doit être ouverte), Ajouter comme pub  →  Ctrl+S et envoi comme ci-dessus,
                    puis « Vider » le groupe sur le portail.
  Tout est rangé dans le dossier DetecteurPub de l'utilisateur : base/ (la base en cours), captations/,
  a_envoyer/base_corrigee.zip et sauvegardes/ (les 5 dernières, les plus anciennes s'effacent seules).
  L'ancien mode (carte SD branchée sur le PC) reste disponible dans le menu Fichier.

NETTOYER UNE CARTE SD (v0.9.55) : « Fichier → Nettoyer une carte SD », la carte branchee sur le PC.
  Tout est classe : vert garde (base, sons, voix, mot-cle, TV, historique), orange inutile (coche),
  bleu captations pas encore traitees, gris inconnu (non coches). Avant d'effacer, toute la carte est
  copiee dans DetecteurPub/sauvegardes_carte_sd (3 dernieres copies) ; « Remettre une copie complete »
  revient a l'etat d'avant.

CAPTATIONS (v0.6.3) : « Fichier -> Ouvrir une captation / importer un WAV » (Ctrl+I) ouvre un
long enregistrement (/pub/captations/cap_NNN.wav fait par le bouton CAPTER de la carte, ou
n'importe quel WAV). On y repere chaque pub avec D / F, puis « Ajouter comme pub » : les
empreintes sont calculees ici EXACTEMENT comme sur la carte (meme algorithme, verifie contre le
code C++) et ajoutees a base.bin / infos.bin, avec le son dans /pub/audio/.

Python 3.8 ou plus. Captations : pip install numpy (scipy en plus pour importer des WAV d'une autre
frequence que 16 kHz). Apparence Windows 11 : pip install sv-ttk (facultatif ;
sans lui, la fenetre garde l'apparence Windows classique).
Lancement : double-clic, ou   python gestion_pubs.py   [dossier de la carte SD]
"""

VERSION = "v0.9.58"   # meme numero que le firmware (interface.h : PUB_VERSION)
import os
import sys
import struct
import shutil
import tempfile
import time
import wave
import array
import subprocess
import copy
import zipfile
import math

try:
    import numpy as np          # facultatif : seulement pour les captations et l'import de WAV
except ImportError:
    np = None

# ---------------------------------------------------------------- format (base.h / base.cpp)
MAGIE_BASE = 0x31425550          # "PUB1"
MAGIE_INFOS = 0x32464E49         # "INF2"
MAX_BLOCS = 512
CAP = 400000
IPS = 8000 / 256                 # 31,25 colonnes d'empreinte par seconde
FS_WAV = 8000                    # sons des pubs : 8 kHz
PUB = 0

S_ENTETE = struct.Struct('<IIII')            # magie, nb_entrees, max_numero, reserve
S_BLOC = struct.Struct('<BBHIII')            # actif, type, duree_s, nb, vus, numero (16 octets)
S_ENTREE = struct.Struct('<IHH')             # h, bloc, t (8 octets)
S_INFO = struct.Struct('<28sBBBBIII')        # nom, chaine, audio, origine, drapeaux, distant, appris_le, vu_le (44 octets)
TAILLE_FIXE = S_ENTETE.size + MAX_BLOCS * S_BLOC.size
TAILLE_INFOS = 4 + MAX_BLOCS * S_INFO.size

CHAINES = [
    "?",
    "TF1", "France 2", "France 3", "France 4", "France 5", "M6", "Arte", "LCP", "W9", "TMC",
    "TFX", "Gulli", "BFMTV", "CNews", "LCI", "franceinfo", "CStar", "T18", "NOVO19", "TF1 Séries Films",
    "L'Équipe", "6ter", "RMC Story", "RMC Découverte", "RMC Life", "Paris Première",
    "Canal+", "BFM Business", "France 24", "Tech & Co", "RMC Sport", "Eurosport 1", "beIN Sports 1",
    "Téva", "Série Club", "13ème Rue", "Comédie+", "Canal J", "TiJi", "M6 Music", "RMC Mécanic",
    "Sport en France", "Figaro Live", "Brut", "Molotov", "Chaîne locale", "Autre"]
ORIGINES = {0: "bouton", 1: "import", 2: "auto", 3: "base distante"}

# v0.9.54 : dossiers de travail fixes (une seule base en cours, une seule captation en cours)
DOSSIER_PC = os.path.join(os.path.expanduser('~'), 'DetecteurPub')
DOSSIER_BASE = os.path.join(DOSSIER_PC, 'base')                 # base ZIP depliee (base\pub\...)
DOSSIER_CAPT = os.path.join(DOSSIER_PC, 'captations')           # captations ZIP depliees
DOSSIER_ENVOI = os.path.join(DOSSIER_PC, 'a_envoyer')           # base_corrigee.zip, a envoyer au portail
DOSSIER_SAUV = os.path.join(DOSSIER_PC, 'sauvegardes')          # copies avant chaque enregistrement
FICHIER_ENVOI = os.path.join(DOSSIER_ENVOI, 'base_corrigee.zip')
MARQUE_ENVOI = os.path.join(DOSSIER_ENVOI, 'pas_encore_envoyee.txt')
SAUV_GARDEES = 5


def dossier_telechargements():
    """ou le navigateur range les ZIP du portail"""
    for n in ('Downloads', 'Téléchargements'):
        d = os.path.join(os.path.expanduser('~'), n)
        if os.path.isdir(d):
            return d
    return os.path.expanduser('~')


def vider_dossier(d):
    """supprime le contenu d'un de NOS dossiers de travail (jamais ailleurs)"""
    assert os.path.abspath(d).startswith(os.path.abspath(DOSSIER_PC) + os.sep)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d, exist_ok=True)


FICHIER_OUVERTS = os.path.join(DOSSIER_PC, 'deja_ouverts.txt')   # v0.9.56 : les .zip du portail deja ouverts


def cle_fichier(c):
    try:
        return "%s|%d|%d" % (os.path.abspath(c), os.path.getsize(c), int(os.path.getmtime(c)))
    except OSError:
        return ""


def deja_ouvert(c):
    try:
        with open(FICHIER_OUVERTS, encoding='utf-8') as f:
            return cle_fichier(c) in (l.strip() for l in f)
    except OSError:
        return False


def noter_ouvert(c):
    try:
        os.makedirs(DOSSIER_PC, exist_ok=True)
        lignes = []
        if os.path.isfile(FICHIER_OUVERTS):
            with open(FICHIER_OUVERTS, encoding='utf-8') as f:
                lignes = [l.strip() for l in f if l.strip()]
        lignes = (lignes + [cle_fichier(c)])[-50:]
        with open(FICHIER_OUVERTS, 'w', encoding='utf-8') as f:
            f.write("\n".join(lignes) + "\n")
    except OSError:
        pass


def zip_recent(genre):
    """v0.9.56 : le .zip du portail le plus recent des Telechargements.
    genre 'base' : contient base.bin (« Carte → PC ») ; 'cap' : des .wav sans base.bin (« Tout télécharger »)"""
    d = dossier_telechargements()
    try:
        zips = [os.path.join(d, n) for n in os.listdir(d) if n.lower().endswith('.zip')]
        zips.sort(key=os.path.getmtime, reverse=True)
    except OSError:
        return None
    for c in zips[:40]:
        try:
            with zipfile.ZipFile(c) as a:
                noms = [i.filename.replace('\\', '/') for i in a.infolist()]
        except (OSError, zipfile.BadZipFile):
            continue
        if genre == 'base' and 'base.bin' in noms:
            return c
        if genre == 'cap' and 'base.bin' not in noms and any(n.lower().endswith('.wav') and '/' not in n for n in noms):
            return c
    return None


def cartes_sd():
    """v0.9.56 : les cartes SD du detecteur branchees sur le PC (racines qui contiennent pub/base.bin)"""
    racines = []
    if sys.platform.startswith('win'):
        try:
            import ctypes
            masque = ctypes.windll.kernel32.GetLogicalDrives()
            ctypes.windll.kernel32.SetErrorMode(1)     # pas de fenetre « inserez un disque » sur un lecteur vide
        except Exception:
            masque = (1 << 26) - 1
        racines = ["%s:\\" % chr(65 + i) for i in range(2, 26) if masque & (1 << i)]
    else:
        for base in ('/media', '/run/media', '/Volumes', '/mnt'):
            for r, ds, _ in os.walk(base):
                racines += [os.path.join(r, x) for x in ds]
                if r.count(os.sep) - base.count(os.sep) >= 1:
                    ds[:] = []
    trouvees = []
    for r in racines:
        try:
            if os.path.isfile(os.path.join(r, 'pub', 'base.bin')) or os.path.isfile(os.path.join(r, 'pub', 'base.new')):
                trouvees.append(r)
        except OSError:
            pass
    return trouvees


def ouvrir_explorateur(d):
    try:
        if sys.platform.startswith('win'):
            os.startfile(d)
        else:
            subprocess.Popen(['xdg-open' if sys.platform.startswith('linux') else 'open', d])
    except OSError:
        pass


def nom_utf8(octets):
    """nom de la fiche (28 octets, termine par 0) -> texte"""
    o = octets.split(b'\0', 1)[0]
    return o.decode('utf-8', errors='replace')


def octets_nom(texte):
    """texte -> 28 octets UTF-8 (27 au plus + 0), sans couper une lettre accentuee"""
    o = texte.encode('utf-8')[:27]
    while o:
        try:
            o.decode('utf-8')
            break
        except UnicodeDecodeError:
            o = o[:-1]
    return o.ljust(28, b'\0')


class Pub:
    """un bloc de la bibliotheque (identifiant = case 0..511 sur la carte)"""
    def __init__(self, ident, bloc, info):
        self.id = ident
        self.actif, self.type, self.duree_s, self.nb, self.vus, self.numero = bloc
        nom, self.chaine, self.audio, self.origine, self.drapeaux, self.distant, self.appris_le, self.vu_le = info
        self.nom = nom_utf8(nom) or "Pub n°%d" % self.numero
        if self.chaine >= len(CHAINES):
            self.chaine = 0

    @property
    def a_verifier(self):
        return bool(self.drapeaux & 1)


class Bibliotheque:
    """base.bin + infos.bin + sons, lus et reecrits a l'identique de la carte"""

    def __init__(self, racine):
        racine = os.path.abspath(racine)
        if os.path.basename(racine).lower() != 'pub' and os.path.isdir(os.path.join(racine, 'pub')):
            racine = os.path.join(racine, 'pub')
        self.dossier = racine                       # .../pub
        self.audio = os.path.join(racine, 'audio')
        self.modifie = False
        self.sons_coupes = {}                       # numero -> (echantillons, debut_s) a ecrire
        self.sons_suppr = set()                     # numeros dont le son est a retirer
        self.source = None
        self.lire()

    # ---------------- lecture
    def lire(self):
        base_new = os.path.join(self.dossier, 'base.new')
        base_bin = os.path.join(self.dossier, 'base.bin')
        donnees = None
        # comme la carte : base.new entier = la sauvegarde la plus recente
        for chemin in (base_new, base_bin):
            if not os.path.isfile(chemin):
                continue
            d = open(chemin, 'rb').read()
            if self._valide(d):
                donnees, self.source = d, os.path.basename(chemin)
                break
        if donnees is None:
            raise ValueError("Pas de bibliothèque valide (base.bin) dans\n%s" % self.dossier)
        magie, n, self.max_numero, self.reserve = S_ENTETE.unpack_from(donnees, 0)
        blocs = [S_BLOC.unpack_from(donnees, S_ENTETE.size + i * S_BLOC.size) for i in range(MAX_BLOCS)]
        # les entrees restent dans l'ordre de la carte (triees par code, bloc, instant)
        self.entrees = list(S_ENTREE.iter_unpack(donnees[TAILLE_FIXE:TAILLE_FIXE + n * S_ENTREE.size]))
        infos = [(b'\0' * 28, 0, 0, 0, 0, 0, 0, 0)] * MAX_BLOCS
        for chemin in (os.path.join(self.dossier, 'infos.new'), os.path.join(self.dossier, 'infos.bin')):
            if os.path.isfile(chemin) and os.path.getsize(chemin) == TAILLE_INFOS:
                d = open(chemin, 'rb').read()
                if struct.unpack_from('<I', d, 0)[0] == MAGIE_INFOS:
                    infos = [S_INFO.unpack_from(d, 4 + i * S_INFO.size) for i in range(MAX_BLOCS)]
                    break
        self.pubs = {i: Pub(i, blocs[i], infos[i]) for i in range(MAX_BLOCS) if blocs[i][0]}

    @staticmethod
    def _valide(d):
        if len(d) < TAILLE_FIXE:
            return False
        magie, n, _, _ = S_ENTETE.unpack_from(d, 0)
        return magie == MAGIE_BASE and n <= CAP and len(d) == TAILLE_FIXE + n * S_ENTREE.size

    # ---------------- sons
    def chemin_son(self, numero):
        return os.path.join(self.audio, '%d.wav' % numero)

    def son_present(self, pub):
        if pub.numero in self.sons_suppr:
            return False
        if pub.numero in self.sons_coupes:
            return True
        c = self.chemin_son(pub.numero)
        return os.path.isfile(c) and os.path.getsize(c) > 44

    def echantillons(self, pub):
        """son de la pub (array 'h'), decoupe en attente comprise ; None si absent"""
        if pub.numero in self.sons_coupes:
            return self.sons_coupes[pub.numero]
        if not self.son_present(pub):
            return None
        c = self.chemin_son(pub.numero)
        a = array.array('h')
        try:
            with wave.open(c, 'rb') as w:
                a.frombytes(w.readframes(w.getnframes()))
        except (wave.Error, EOFError):
            d = open(c, 'rb').read()[44:]               # en-tete abime : donnees brutes
            a.frombytes(d[:len(d) // 2 * 2])
        if sys.byteorder == 'big':
            a.byteswap()
        return a

    # ---------------- modifications (en memoire)
    def marquer_ok(self, pub, ok=True):
        if ok:
            pub.drapeaux &= ~1 & 0xFF
        else:
            pub.drapeaux |= 1
        self.modifie = True

    def renommer(self, pub, nom, chaine):
        nom = nom.strip()
        if nom:
            pub.nom = octets_nom(nom).split(b'\0', 1)[0].decode('utf-8')
        if 0 <= chaine < len(CHAINES):
            pub.chaine = chaine
        pub.drapeaux &= ~1 & 0xFF                      # regardee : plus « a verifier » (comme la carte)
        self.modifie = True

    def supprimer(self, pub):
        self.entrees = [e for e in self.entrees if e[1] != pub.id]
        self.sons_coupes.pop(pub.numero, None)
        self.sons_suppr.add(pub.numero)
        del self.pubs[pub.id]
        self.modifie = True

    def decouper(self, pub, debut_s, fin_s):
        """ne garde que le passage [debut, fin] (empreintes ET son), comme garder_plage"""
        d_ds, f_ds = int(round(debut_s * 10)), int(round(fin_s * 10))
        if f_ds <= d_ds:
            raise ValueError("La fin doit être après le début")
        t0, t1 = int(d_ds * IPS / 10), int(f_ds * IPS / 10)
        garde = []
        for h, b, t in self.entrees:
            if b != pub.id:
                garde.append((h, b, t))
            elif t0 <= t <= t1:
                garde.append((h, b, t - t0))           # meme decalage pour tout le bloc : l'ordre reste trie
        self.entrees = garde
        pub.nb = sum(1 for e in self.entrees if e[1] == pub.id)
        pub.duree_s = int((t1 - t0) / IPS + 0.5)
        son = self.echantillons(pub)
        if son is not None:
            s0, s1 = d_ds * FS_WAV // 10, min(f_ds * FS_WAV // 10, len(son))
            self.sons_coupes[pub.numero] = son[s0:s1]
        self.modifie = True
        if pub.nb == 0:
            self.supprimer(pub)
            return False
        return True

    # ---------------- B2 : doublons et fusion de passages d'une meme pub
    def doublons(self, seuil_pct=30, mini=10):
        """paires de pubs qui sont en fait la meme (memes codes, decales dans le temps) :
        [(id_a, id_b, votes, pourcent, decalage_s)] triees par ressemblance. decalage : t_a ~ t_b + decalage."""
        index, par = {}, {}
        for h, b, t in self.entrees:
            m = h & MASQUE
            index.setdefault(m, []).append((b, t))
            par.setdefault(b, []).append((m, t))
        res = []
        for a in sorted(par):
            votes, ds = {}, {}
            for m, ta in par[a]:
                for b, tb in index[m][:48]:
                    if b <= a:
                        continue
                    d = ta - tb
                    k = (b, d >> 1)
                    votes[k] = votes.get(k, 0) + 1
                    ds.setdefault(k, []).append(d)
            meilleur = {}
            for (b, k), n in votes.items():
                if n > meilleur.get(b, (0, 0))[0]:
                    meilleur[b] = (n, k)
            for b, (n, k) in meilleur.items():
                pct = 100.0 * n / max(1, min(len(par[a]), len(par[b])))
                if n >= mini and pct >= seuil_pct and a in self.pubs and b in self.pubs:
                    d = sorted(ds[(b, k)])[len(ds[(b, k)]) // 2]
                    res.append((a, b, n, pct, d / IPS))
        return sorted(res, key=lambda r: -r[3])

    def _decalage(self, ga, au):
        """decalage d (en colonnes) tel que t_garde ~ t_autre + d, par vote majoritaire"""
        idx = {}
        for h, b, t in self.entrees:
            if b == au:
                idx.setdefault(h & MASQUE, []).append(t)
        votes = {}
        for h, b, t in self.entrees:
            if b == ga:
                for tb in idx.get(h & MASQUE, ())[:24]:
                    d = t - tb
                    votes.setdefault(d, 0)
                    votes[d] += 1
        if not votes:
            return 0
        d0 = max(votes, key=lambda d: votes[d] + votes.get(d - 1, 0) + votes.get(d + 1, 0))
        return d0

    def fusionner(self, garde, autre):
        """versions de la meme pub (autre passage, autre bruit) : les codes de `autre`, recales dans le temps,
        sont AJOUTES a `garde` (sans doublon), puis `autre` disparait. Plus de codes = reconnaissance plus robuste.
        Renvoie le nombre de codes ajoutes."""
        d = self._decalage(garde.id, autre.id)
        deja = {}
        for h, b, t in self.entrees:
            if b == garde.id:
                deja.setdefault(h & MASQUE, []).append(t)
        ajout, tmax = [], 0
        for h, b, t in self.entrees:
            if b != autre.id:
                continue
            m, t2 = h & MASQUE, t + d
            if t2 < 0 or t2 > 65535:
                continue
            if any(abs(t2 - x) <= 1 for x in deja.get(m, ())):
                continue
            ajout.append((m, garde.id, t2))
            deja.setdefault(m, []).append(t2)
            tmax = max(tmax, t2)
        if len(self.entrees) + len(ajout) > CAP:
            raise ValueError("Bibliothèque pleine : fusion impossible.")
        self.entrees = sorted(self.entrees + ajout, key=lambda e: (e[0] & MASQUE, e[1], e[2]))
        garde.duree_s = max(garde.duree_s, int(tmax / IPS + 0.999))
        garde.vus = garde.vus + autre.vus
        self.supprimer(autre)
        self.modifie = True
        return len(ajout)

    # ---------------- pubs ajoutees depuis une captation (comme ajouter_bloc de la carte)
    def similaire(self, h, t, seuil=10):
        """pub deja connue qui ressemble a ces codes ? -> (numero, votes) ou None. Meme vote que la carte :
        chaque code vote pour (pub, decalage)."""
        index = {}
        for hh, b, tt in self.entrees:
            index.setdefault(hh & MASQUE, []).append((b, tt))
        votes = {}
        for hh, tc in zip(h.tolist(), t.tolist()):
            for b, tb in index.get(hh & MASQUE, ())[:24]:
                d = tc - tb
                for cle in {(b, d >> 1), (b, (d + 1) >> 1)}:
                    votes[cle] = votes.get(cle, 0) + 1
        if not votes:
            return None
        (b, _), n = max(votes.items(), key=lambda kv: kv[1])
        return (self.pubs[b].numero, n) if (n >= seuil and b in self.pubs) else None

    def balayer(self, h, t, seuil=10):
        """C2 : rejoue une captation entiere contre la bibliotheque. Meme vote que la carte (pub, decalage) ;
        renvoie les passages reconnus [(debut_s, numero, nom, votes, duree_s)] dans l'ordre du temps.
        Une pub revue plus loin donne un 2e passage."""
        index = {}
        for hh, b, tt in self.entrees:
            index.setdefault(hh & MASQUE, []).append((b, tt))
        if not index:
            return []
        cles = np.fromiter(index.keys(), dtype=np.int64, count=len(index))
        h = np.asarray(h).astype(np.int64)
        t = np.asarray(t).astype(np.int64)
        garde = np.isin(h & MASQUE, cles)                       # seuls les codes connus de la bibliotheque comptent
        votes = {}
        for hh, tc in zip((h[garde] & MASQUE).tolist(), t[garde].tolist()):
            for b, tb in index[hh][:24]:
                d = tc - tb
                for cle in {(b, d >> 1), (b, (d + 1) >> 1)}:
                    votes[cle] = votes.get(cle, 0) + 1
        brut = sorted((b, k, n) for (b, k), n in votes.items() if n >= seuil and b in self.pubs)
        trouves = []
        i = 0
        while i < len(brut):                                    # regroupe les decalages voisins d'un meme passage
            j = i
            meilleur = brut[i]
            while j + 1 < len(brut) and brut[j + 1][0] == brut[i][0] and brut[j + 1][1] - brut[j][1] <= 2:
                j += 1
                if brut[j][2] > meilleur[2]:
                    meilleur = brut[j]
            b, k, n = meilleur
            p = self.pubs[b]
            trouves.append((max(0.0, 2 * k / IPS), p.numero, p.nom, n, float(p.duree_s)))
            i = j + 1
        return sorted(trouves)

    def ajouter_pub(self, h, t, t0, t1, son8, nom='', chaine=0):
        """ajoute une pub : codes d'apprentissage (sans bit REQUETE) dont la colonne d'ancrage est dans [t0, t1],
        dates depuis t0 ; son 8 kHz (numpy int16). Renvoie la Pub creee."""
        libre = next((i for i in range(MAX_BLOCS) if i not in self.pubs), None)
        if libre is None:
            raise ValueError("Bibliothèque pleine (%d pubs). Supprime-en une." % MAX_BLOCS)
        sel = (t >= t0) & (t <= t1) & ((h & REQUETE) == 0)
        hh, tt = h[sel], t[sel].astype(np.int64) - t0
        garde = tt <= 65535
        hh, tt = hh[garde], tt[garde]
        if len(hh) == 0:
            raise ValueError("Aucune empreinte dans ce passage (silence ou trop court).")
        if len(self.entrees) + len(hh) > CAP:
            raise ValueError("Bibliothèque pleine (%d empreintes sur %d)." % (len(self.entrees), CAP))
        self.max_numero += 1
        numero = self.max_numero
        nouvelles = [(int(a) & MASQUE, libre, int(b)) for a, b in zip(hh.tolist(), tt.tolist())]
        self.entrees = sorted(self.entrees + nouvelles, key=lambda e: (e[0] & MASQUE, e[1], e[2]))
        duree = int((t1 - t0) / IPS + 0.5)
        bloc = (1, PUB, duree, len(nouvelles), 0, numero)
        info = (octets_nom(nom.strip() or "Pub n°%d" % numero), chaine if 0 <= chaine < len(CHAINES) else 0, 1, 1,
                0, 0, int(time.time()), 0)                    # origine « import », deja ecoutee (pas « a verifier »)
        self.pubs[libre] = Pub(libre, bloc, info)
        self.sons_suppr.discard(numero)
        self.sons_coupes[numero] = en_array(son8)
        self.modifie = True
        return self.pubs[libre]

    def remplacer_pub(self, pub, h, t, t0, t1, son8):
        """v0.9.56 : la pub garde sa case, son numero, son nom, sa chaine et ses detections ;
        empreintes et son viennent du passage [t0, t1] (comme ajouter_pub). Marquee « OK »."""
        sel = (t >= t0) & (t <= t1) & ((h & REQUETE) == 0)
        hh, tt = h[sel], t[sel].astype(np.int64) - t0
        garde = tt <= 65535
        hh, tt = hh[garde], tt[garde]
        if len(hh) == 0:
            raise ValueError("Aucune empreinte dans ce passage (silence ou trop court).")
        autres = [e for e in self.entrees if e[1] != pub.id]
        if len(autres) + len(hh) > CAP:
            raise ValueError("Bibliothèque pleine (%d empreintes sur %d)." % (len(autres), CAP))
        nouvelles = [(int(a) & MASQUE, pub.id, int(b)) for a, b in zip(hh.tolist(), tt.tolist())]
        self.entrees = sorted(autres + nouvelles, key=lambda e: (e[0] & MASQUE, e[1], e[2]))
        pub.nb = len(nouvelles)
        pub.duree_s = int((t1 - t0) / IPS + 0.5)
        pub.drapeaux &= ~1 & 0xFF
        self.sons_suppr.discard(pub.numero)
        self.sons_coupes[pub.numero] = en_array(son8)
        self.modifie = True
        return pub

    # ---------------- annuler (Ctrl+Z) : photo de l'etat en memoire
    def instantane(self):
        return (list(self.entrees), copy.deepcopy(self.pubs), dict(self.sons_coupes), set(self.sons_suppr), self.modifie,
                self.max_numero)

    def revenir(self, photo):
        self.entrees, self.pubs, self.sons_coupes, self.sons_suppr, self.modifie, self.max_numero = photo

    @property
    def dossier_sauvegardes(self):
        return DOSSIER_SAUV                         # v0.9.54 : un seul endroit, sur le PC

    def sauvegardes(self):
        """chemins des copies, la plus recente d'abord (+ les anciennes /pub/sauvegardes_pc de la carte)"""
        liste = []
        for d in (DOSSIER_SAUV, os.path.join(self.dossier, 'sauvegardes_pc')):
            if os.path.isdir(d):
                liste += [os.path.join(d, n) for n in os.listdir(d) if os.path.isfile(os.path.join(d, n, 'base.bin'))]
        return sorted(liste, key=lambda c: os.path.basename(c), reverse=True)

    @staticmethod
    def elaguer_sauvegardes():
        """v0.9.54 : seules les SAUV_GARDEES copies les plus recentes du PC sont gardees"""
        if not os.path.isdir(DOSSIER_SAUV):
            return
        noms = sorted((n for n in os.listdir(DOSSIER_SAUV) if os.path.isdir(os.path.join(DOSSIER_SAUV, n))), reverse=True)
        for n in noms[SAUV_GARDEES:]:
            shutil.rmtree(os.path.join(DOSSIER_SAUV, n), ignore_errors=True)

    def restaurer(self, chemin):
        """remet une copie de securite en place (bibliotheque + sons), puis relit"""
        src = chemin
        for f in ('base.new', 'infos.new'):
            c = os.path.join(self.dossier, f)
            if os.path.isfile(c):
                os.remove(c)
        for f in ('base.bin', 'infos.bin'):
            if os.path.isfile(os.path.join(src, f)):
                shutil.copy2(os.path.join(src, f), os.path.join(self.dossier, f))
        sa = os.path.join(src, 'audio')
        if os.path.isdir(sa):
            os.makedirs(self.audio, exist_ok=True)
            for f in os.listdir(sa):
                shutil.copy2(os.path.join(sa, f), os.path.join(self.audio, f))
        self.sons_coupes.clear()
        self.sons_suppr.clear()
        self.modifie = False
        self.lire()

    # ---------------- ecriture sur la carte
    def enregistrer(self):
        """copie de securite, puis ecriture ; renvoie le dossier de la copie"""
        horodate = time.strftime('%Y%m%d_%H%M%S')
        sauve = os.path.join(DOSSIER_SAUV, horodate)
        os.makedirs(os.path.join(sauve, 'audio'), exist_ok=True)
        for nom in ('base.bin', 'infos.bin', 'base.new', 'infos.new'):
            c = os.path.join(self.dossier, nom)
            if os.path.isfile(c):
                shutil.copy2(c, os.path.join(sauve, nom))
        # nombre d'entrees de chaque bloc : recompte
        compte = {}
        for e in self.entrees:
            compte[e[1]] = compte.get(e[1], 0) + 1
        for p in self.pubs.values():
            p.nb = compte.get(p.id, 0)
        # base.bin
        morceaux = [S_ENTETE.pack(MAGIE_BASE, len(self.entrees), self.max_numero, self.reserve)]
        for i in range(MAX_BLOCS):
            p = self.pubs.get(i)
            morceaux.append(S_BLOC.pack(1, p.type, p.duree_s, p.nb, p.vus, p.numero) if p else bytes(S_BLOC.size))
        morceaux.append(b''.join(S_ENTREE.pack(*e) for e in self.entrees))
        base = b''.join(morceaux)
        # infos.bin
        fiches = [struct.pack('<I', MAGIE_INFOS)]
        for i in range(MAX_BLOCS):
            p = self.pubs.get(i)
            if p:
                audio = 1 if self.son_present(p) else 0
                fiches.append(S_INFO.pack(octets_nom(p.nom), p.chaine, audio, p.origine, p.drapeaux,
                                          p.distant, p.appris_le, p.vu_le))
            else:
                fiches.append(bytes(S_INFO.size))
        infos = b''.join(fiches)
        self._ecrire(os.path.join(self.dossier, 'base.bin'), base)
        self._ecrire(os.path.join(self.dossier, 'infos.bin'), infos)
        # la carte prefere base.new / infos.new s'ils existent : on les retire
        for nom in ('base.new', 'infos.new'):
            c = os.path.join(self.dossier, nom)
            if os.path.isfile(c):
                os.remove(c)
        # sons : les supprimes et les decoupes partent dans la copie de securite
        for num in self.sons_suppr:
            c = self.chemin_son(num)
            if os.path.isfile(c):
                shutil.move(c, os.path.join(sauve, 'audio', '%d.wav' % num))
        for num, son in self.sons_coupes.items():
            c = self.chemin_son(num)
            if os.path.isfile(c):
                shutil.copy2(c, os.path.join(sauve, 'audio', '%d.wav' % num))
            ecrire_wav(c, son)
        self.sons_suppr.clear()
        self.sons_coupes.clear()
        self.modifie = False
        self.source = 'base.bin'
        self.elaguer_sauvegardes()
        return sauve

    @staticmethod
    def _ecrire(chemin, donnees):
        tmp = chemin + '.pc'
        with open(tmp, 'wb') as f:
            f.write(donnees)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, chemin)


def ecrire_wav(chemin, son, fs=FS_WAV):
    """son : array('h'), liste, ou tableau numpy int16"""
    if np is not None and isinstance(son, np.ndarray):
        donnees = son.astype('<i2').tobytes()
    else:
        a = array.array('h', son)
        if sys.byteorder == 'big':
            a.byteswap()
        donnees = a.tobytes()
    os.makedirs(os.path.dirname(chemin), exist_ok=True)
    with wave.open(chemin, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(donnees)


def gain_auto(son):
    """volume remonte (le micro enregistre la tele bas) : le 0,1 % le plus fort est ignore"""
    if not son:
        return 1.0
    pas = max(1, len(son) // 40000)                   # un echantillon sur "pas" suffit
    v = sorted(abs(x) for x in son[::pas])
    ref = v[min(len(v) - 1, int(len(v) * 0.999))] or 1
    return max(1.0, min(24.0, 29000.0 / ref))


def amplifier(son, g):
    return array.array('h', (max(-32768, min(32767, int(x * g))) for x in son))


# ---------------------------------------------------------------- lecture du son
class Lecteur:
    """joue un WAV (Windows : winsound, sinon aplay / afplay)"""

    def __init__(self):
        self.proc = None
        self.tmp = os.path.join(tempfile.gettempdir(), 'detecteur_pub_ecoute.wav')

    def jouer(self, son, fs=FS_WAV):
        self.arreter()
        ecrire_wav(self.tmp, son, fs)
        if sys.platform.startswith('win'):
            import winsound
            winsound.PlaySound(self.tmp, winsound.SND_FILENAME | winsound.SND_ASYNC)
        else:
            for cmd in (['afplay'], ['aplay', '-q'], ['paplay']):
                try:
                    self.proc = subprocess.Popen(cmd + [self.tmp])
                    break
                except OSError:
                    continue

    def arreter(self):
        if sys.platform.startswith('win'):
            import winsound
            winsound.PlaySound(None, 0)
        elif self.proc:
            self.proc.terminate()
            self.proc = None




# ---------------------------------------------------------------- empreintes (port numpy de empreinte.cpp + fft.cpp)
# Meme algorithme et meme arithmetique en float32 que la carte : les codes calcules ici sont
# ceux que la carte aurait appris (verifie contre le vrai code C++ sur une captation).
# Reglages = valeurs par defaut de empreinte.h (densite 15 / 40 pics par seconde).
EMP_N, EMP_SAUT, EMP_B0, EMP_NB = 512, 256, 16, 224
EMP_DT_MAX, EMP_DF_MAX = 40, 48
EMP_EVENTAIL, EMP_EVENTAIL_REQ, EMP_EVENTAIL_FAIBLE = 4, 8, 12
EMP_MAX_PAR_COL, EMP_MAX_TOUS_COL = 4, 8
EMP_DENSITE, EMP_DENSITE_FAIBLE = 15, 40
EMP_CHAUFFE = int(2 * IPS)                       # 62 colonnes sans code au demarrage
REQUETE, FAIBLE, MASQUE = 0x80000000, 0x40000000, 0x003FFFFF
_EMP = {}


def _emp_tables():
    """tables de rotation / inversion de la FFT, filtre de decimation, fenetre (comme init())"""
    if _EMP:
        return _EMP
    F, N = np.float32, EMP_N
    _EMP['inv'] = np.array([int(format(i, '09b')[::-1], 2) for i in range(N)])
    ang = [-2.0 * 3.14159265358979323846 * i / N for i in range(N // 2)]
    _EMP['cos'] = np.array([math.cos(a) for a in ang], dtype=F)
    _EMP['sin'] = np.array([math.sin(a) for a in ang], dtype=F)
    nf, fc, h = 31, 3600.0 / 16000.0, []
    for i in range(nf):
        m = i - (nf - 1) / 2.0
        v = 2 * fc if m == 0 else math.sin(2 * 3.14159265358979 * fc * m) / (3.14159265358979 * m)
        v *= 0.54 - 0.46 * math.cos(2 * 3.14159265358979 * i / (nf - 1))
        h.append(v)
    s = 0.0
    for v in h:
        s += v
    _EMP['coef'] = [F(float(F(v)) / s) for v in h]
    pi32 = F(3.14159265)
    fen = np.empty(N, dtype=F)
    for i in range(N):
        arg = F(F(2) * pi32) * F(i) / F(N)
        fen[i] = F(0.5) - F(0.5) * F(math.cos(float(arg)))
    _EMP['fen'] = fen
    return _EMP


def _emp_fft(re, im):
    """FFT radix 2 de 512 points sur chaque ligne, memes operations que fft.cpp"""
    t, N = _emp_tables(), EMP_N
    c = re.shape[0]
    re, im = re[:, t['inv']], im[:, t['inv']]
    taille = 2
    while taille <= N:
        moitie, pas = taille // 2, N // taille
        wr, wi = t['cos'][np.arange(moitie) * pas], t['sin'][np.arange(moitie) * pas]
        r, i_ = re.reshape(c, N // taille, taille), im.reshape(c, N // taille, taille)
        ar, br, ai, bi = r[:, :, :moitie], r[:, :, moitie:], i_[:, :, :moitie], i_[:, :, moitie:]
        tr = br * wr - bi * wi
        ti = br * wi + bi * wr
        nr, ni = np.empty_like(r), np.empty_like(i_)
        nr[:, :, moitie:], ni[:, :, moitie:] = ar - tr, ai - ti
        nr[:, :, :moitie], ni[:, :, :moitie] = ar + tr, ai + ti
        re, im = nr.reshape(c, N), ni.reshape(c, N)
        taille <<= 1
    return re, im


def _emp_decimer(x16):
    """16 kHz int16 -> 8 kHz float32 (filtre passe-bas de 31 coefficients, un echantillon sur deux)"""
    F, t = np.float32, _emp_tables()
    x = x16.astype(F) * F(1.0 / 32768.0)
    n8 = len(x) // 2
    xp = np.concatenate([np.zeros(30, F), x])
    y = np.zeros(n8, F)
    for k in range(31):
        y = y + t['coef'][k] * xp[1 + k:1 + k + 2 * n8:2][:n8]
    return y


def _emp_spectre(y, progress=None):
    """spectrogramme en dB : une colonne toutes les 256 valeurs, bins 16..239"""
    F, t = np.float32, _emp_tables()
    nc = len(y) // EMP_SAUT
    yp = np.concatenate([np.zeros(EMP_SAUT, F), y])
    db = np.empty((nc, EMP_NB), F)
    pas = 1024
    for c0 in range(0, nc, pas):
        c1 = min(nc, c0 + pas)
        idx = np.arange(c0, c1)[:, None] * EMP_SAUT + np.arange(EMP_N)[None, :]
        fr = yp[idx] * t['fen']
        re, im = _emp_fft(fr, np.zeros_like(fr))
        r, i_ = re[:, EMP_B0:EMP_B0 + EMP_NB], im[:, EMP_B0:EMP_B0 + EMP_NB]
        p = r * r + i_ * i_ + F(1e-9)
        db[c0:c1] = F(10) * np.log10(p.astype(np.float64)).astype(F)
        if progress:
            progress(0.5 * c1 / max(1, nc))
    return db


def _emp_codes(db, tous=False, progress=None):
    """blanchiment, pics locaux (seuil adaptatif), paires de pics -> codes (liste h, liste t)"""
    F = np.float32
    nc = db.shape[0]
    hs, ts = [], []
    if nc < 7:
        return hs, ts
    blanc, moy, k_blanc = np.empty_like(db), None, F(0.02)
    for t in range(nc):
        if moy is None:
            moy = db[t].copy()
        moy = moy + (db[t] - moy) * k_blanc
        blanc[t] = db[t] - moy
    # maxima locaux (voisinage 7 x 7, a egalite le premier gagne), independants du seuil
    v = blanc[3:nc - 3, 3:EMP_NB - 3]
    est_max = np.ones(v.shape, dtype=bool)
    for dk in range(-3, 4):
        for df in range(-3, 4):
            if dk == 0 and df == 0:
                continue
            w = blanc[3 + dk:nc - 3 + dk, 3 + df:EMP_NB - 3 + df]
            est_max &= ~((w >= v) if (dk < 0 or (dk == 0 and df < 0)) else (w > v))
    est_max &= (v >= F(2.0)) & (db[3:nc - 3, 3:EMP_NB - 3] >= F(-38.0))
    rows, cols = np.nonzero(est_max)
    vals = v[rows, cols]
    debuts = np.searchsorted(rows, np.arange(nc - 5))
    ips32 = F(IPS)
    seuil, seuil_f = F(8.0), F(5.0)
    cpt_pics = cpt_faibles = cpt_cols = 0
    rp = []                                      # pics en attente : [colonne, bin, fort, cibles fortes, cibles]
    maxc = max(EMP_MAX_TOUS_COL, EMP_MAX_PAR_COL)
    for col in range(nc):
        if col >= 6:
            tc = col - 3
            r0, r1 = debuts[tc - 3], debuts[tc - 2]
            s_min = seuil_f if (seuil_f < seuil and EMP_DENSITE_FAIBLE > 0) else seuil
            bs, vs = (cols[r0:r1] + 3), vals[r0:r1]
            ok = vs >= s_min
            cand = []
            for b, vv in zip(bs[ok].tolist(), vs[ok].tolist()):
                if len(cand) < maxc:
                    cand.append([b, vv])
                else:
                    pire = 0
                    for q in range(1, maxc):
                        if cand[q][1] < cand[pire][1]:
                            pire = q
                    if vv > cand[pire][1]:
                        cand[pire] = [b, vv]
            sf = float(seuil)
            fort = []
            for q in range(len(cand)):
                rang = sum(1 for r in range(len(cand))
                           if cand[r][1] > cand[q][1] or (cand[r][1] == cand[q][1] and r < q))
                fort.append(cand[q][1] >= sf and rang < EMP_MAX_PAR_COL)
            for q in sorted(range(len(cand)), key=lambda q: cand[q][0]):
                f, fo = cand[q][0], fort[q]
                for a in reversed(rp):
                    if a[0] + EMP_DT_MAX < tc:
                        break
                    if a[0] == tc:
                        continue
                    df = f - a[1]
                    if df > EMP_DF_MAX or df < -EMP_DF_MAX:
                        continue
                    ff = a[2] and fo
                    code_fort = ff and a[3] < EMP_EVENTAIL_REQ
                    code_faible = (not code_fort) and a[4] < EMP_EVENTAIL_FAIBLE
                    if (code_fort or code_faible) and a[0] >= EMP_CHAUFFE:
                        h = (a[1] << 14) | (f << 6) | ((tc - a[0]) & 63)
                        if code_faible:
                            h |= REQUETE | FAIBLE
                        elif a[3] >= EMP_EVENTAIL:
                            h |= REQUETE
                        if tous or not (h & REQUETE):
                            hs.append(h)
                            ts.append(a[0])
                    if ff and a[3] < 255:
                        a[3] += 1
                    if a[4] < 255:
                        a[4] += 1
                rp.append([tc, f, fo, 0, 0])
            cpt_pics += sum(fort)
            cpt_faibles += len(cand)
        cpt_cols += 1
        if cpt_cols >= 31:                       # seuils adaptatifs : visent 15 et 40 pics par seconde
            cible = F(max(EMP_DENSITE, 1))
            ecart = (F(cpt_pics) * ips32 / F(cpt_cols) - cible) / cible
            if ecart > 1:
                ecart = F(1)
            seuil = seuil + F(1.2) * ecart
            seuil = F(4) if seuil < 4 else (F(40) if seuil > 40 else seuil)
            if EMP_DENSITE_FAIBLE > 0:
                cf = F(EMP_DENSITE_FAIBLE)
                ef = (F(cpt_faibles) * ips32 / F(cpt_cols) - cf) / cf
                if ef > 1:
                    ef = F(1)
                seuil_f = seuil_f + F(1.2) * ef
                seuil_f = F(2) if seuil_f < 2 else (seuil if seuil_f > seuil else seuil_f)
            cpt_cols = cpt_pics = cpt_faibles = 0
        if progress and col % 512 == 0:
            progress(0.5 + 0.5 * col / nc)
        if col % 2048 == 2047:                    # les vieux pics ne servent plus
            rp = [p for p in rp if p[0] + EMP_DT_MAX >= col - 3]
    return hs, ts


def calculer_empreintes(x16, tous=False, progress=None):
    """x16 : tableau numpy int16, mono, 16 kHz. Renvoie (h, t) : les codes que la carte apprendrait
    (tous=True : aussi les codes de recherche, pour comparer avec la carte). t = colonne d'ancrage."""
    db = _emp_spectre(_emp_decimer(x16), progress)
    hs, ts = _emp_codes(db, tous, progress)
    return np.array(hs, dtype=np.uint32), np.array(ts, dtype=np.uint32)


# ---------------------------------------------------------------- lecture d'un WAV (captation de la carte ou fichier importe)
def exiger_numpy():
    if np is None:
        raise RuntimeError("Cette fonction demande numpy :  pip install numpy")


def lire_wav(chemin):
    """lit un WAV PCM 8/16/24/32 bits ou flottant 32 bits. Tolere l'en-tete d'une captation interrompue
    (carte coupee : la taille annoncee est plus petite que le fichier). -> (mono float32, Hz, canaux, remarque)"""
    exiger_numpy()
    with open(chemin, 'rb') as f:
        d = f.read()
    if d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        raise ValueError("Ce fichier n'est pas un WAV.")
    pos, fmt, debut, taille = 12, None, None, 0
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
        if cid == b'fmt ' and pos + 24 <= len(d):
            tag, ch, fs, _, _, bits = struct.unpack_from('<HHIIHH', d, pos + 8)
            if tag == 0xFFFE and sz >= 26:
                tag = struct.unpack_from('<H', d, pos + 32)[0]
            fmt = (tag, ch, fs, bits)
        elif cid == b'data':
            debut, taille = pos + 8, sz
            break
        pos += 8 + sz + (sz & 1)
    if fmt is None or debut is None:
        raise ValueError("WAV illisible (pas de données audio).")
    tag, ch, fs, bits = fmt
    reste, remarque = len(d) - debut, ''
    if taille == 0 or taille > reste:
        taille, remarque = reste, "en-tête incomplet : fichier lu en entier"
    elif taille < reste:
        suite = d[debut + taille:debut + taille + 4]
        if not all(65 <= c <= 90 or 97 <= c <= 122 or c == 32 for c in suite):   # pas un autre bloc : en-tete perime
            taille, remarque = reste, "en-tête incomplet : fichier lu en entier"
    ch = max(1, ch)
    trame = max(1, bits // 8) * ch
    raw = d[debut:debut + taille - taille % trame]
    if tag == 3 and bits == 32:
        a = np.frombuffer(raw, '<f4').astype(np.float32)
    elif tag == 1 and bits == 8:
        a = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128.0) / 128.0
    elif tag == 1 and bits == 16:
        a = np.frombuffer(raw, '<i2').astype(np.float32) / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        a = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float32) / 8388608.0
    elif tag == 1 and bits == 32:
        a = np.frombuffer(raw, '<i4').astype(np.float32) / 2147483648.0
    else:
        raise ValueError("Format WAV non géré (code %d, %d bits). Convertis-le en WAV 16 bits." % (tag, bits))
    if ch > 1:
        a = a.reshape(-1, ch).mean(axis=1).astype(np.float32)
    return a, fs, ch, remarque


def vers_16k(a, fs):
    """mono float32 (-1..1) -> (int16 a 16 kHz, remarque) : la frequence d'entree de la carte"""
    remarque = ''
    if fs != 16000:
        try:
            from scipy.signal import resample_poly
            g = math.gcd(16000, fs)
            a = resample_poly(a, 16000 // g, fs // g).astype(np.float32)
        except ImportError:
            if fs > 16000:                                   # moyenne glissante puis interpolation
                w = max(1, int(round(fs / 16000.0)))
                c = np.concatenate([[0.0], np.cumsum(a, dtype=np.float64)])
                m = (c[w:] - c[:-w]) / w
                a = m.astype(np.float32)
            n = int(len(a) * 16000.0 / fs)
            a = np.interp(np.arange(n) * (fs / 16000.0), np.arange(len(a)), a).astype(np.float32)
            remarque = "rééchantillonné sans scipy (qualité moyenne : pip install scipy)"
    return np.clip(np.round(a * 32768.0), -32768, 32767).astype(np.int16), remarque


def en_array(x):
    """tableau numpy int16 -> array('h') natif (comme les sons lus sur la carte)"""
    a = array.array('h')
    a.frombytes(np.ascontiguousarray(x).astype('<i2').tobytes())
    if sys.byteorder == 'big':
        a.byteswap()
    return a


def gain_auto_np(x):
    """meme regle que gain_auto, sur un long tableau numpy"""
    if x is None or len(x) == 0:
        return 1.0
    v = np.abs(x[::max(1, len(x) // 200000)].astype(np.int32))
    ref = float(np.percentile(v, 99.9)) or 1.0
    return max(1.0, min(24.0, 29000.0 / ref))


def chaine_captation(chemin):
    """chaine notee par la carte dans index.csv pour cap_NNN.wav (0 si inconnue)"""
    nom = os.path.basename(chemin).lower()
    try:
        if not nom.endswith('.wav'):
            return 0
        if nom.startswith('cap_'):
            num = str(int(nom[4:-4]))          # cap_NNN.wav : numero dans index.csv
        elif nom.startswith('pub_'):
            num = nom                          # v0.9.39 : morceaux de la captation AUTO : nom du fichier dans index.csv
        else:
            return 0
        with open(os.path.join(os.path.dirname(chemin), 'index.csv'), encoding='utf-8', errors='replace') as f:
            for ligne in f:
                p = ligne.strip().split(',')
                if len(p) >= 4 and p[0].lower() == num:
                    c = int(p[3])
                    return c if 0 <= c < len(CHAINES) else 0
    except (OSError, ValueError):
        pass
    return 0


class Captation:
    """un long enregistrement a decouper en pubs : captation de la carte (cap_NNN.wav) ou WAV importe"""
    FS = 16000

    def __init__(self, chemin):
        exiger_numpy()
        self.chemin = chemin
        a, self.fs_origine, self.canaux, self.remarque = lire_wav(chemin)
        self.x, rq = vers_16k(a, self.fs_origine)
        if rq:
            self.remarque = (self.remarque + ' ' + rq).strip()
        if len(self.x) < self.FS * 5:
            raise ValueError("Enregistrement trop court (moins de 5 secondes).")
        self.duree = len(self.x) / float(self.FS)
        self.chaine = chaine_captation(chemin)
        self._codes = None

    def codes(self, progress=None):
        """empreintes de TOUT l'enregistrement, calculees une fois (comme le journal de la carte)"""
        if self._codes is None:
            self._codes = calculer_empreintes(self.x, progress=progress)
        return self._codes

    def passage(self, debut_s, fin_s, progress=None):
        """(h, t, t0, t1, son 8 kHz) du passage [debut, fin] : colonnes d'empreinte et son comme la carte les range"""
        t0, t1 = int(debut_s * IPS), int(fin_s * IPS)
        h, t = self.codes(progress)
        seg = self.x[t0 * EMP_SAUT * 2:t1 * EMP_SAUT * 2]
        n = len(seg) // 2 * 2
        s = seg[0:n:2].astype(np.int32) + seg[1:n:2].astype(np.int32)
        return h, t, t0, t1, ((s + (s < 0)) // 2).astype(np.int16)        # moyenne de 2 echantillons (division C)

    def enveloppe(self, v0, v1, largeur):
        """min et max (entre -1 et 1) de la forme du son sur la vue [v0, v1] s, au plus 'largeur' points"""
        seg = self.x[max(0, int(v0 * self.FS)):min(len(self.x), int(v1 * self.FS))]
        if len(seg) == 0:
            return np.zeros(1), np.zeros(1)
        pas = max(1, len(seg) // max(1, largeur))
        n = len(seg) // pas
        r = seg[:n * pas].reshape(n, pas)
        return r.min(axis=1) / 32768.0, r.max(axis=1) / 32768.0


# ---------------------------------------------------------------- v0.9.56 : captation deja dans la base ?
SUR_VOTES = 50           # au moins 50 empreintes concordantes...
SUR_COUVERTURE = 0.75    # ...reparties sur 75 % de la captation ET 75 % de la pub (meme pub, meme decalage)


def secondes_sonores(x, fs=16000):
    """duree utile d'un enregistrement : les secondes qui ne sont pas du silence (marges avant / apres)"""
    n = len(x) // fs
    if n == 0:
        return len(x) / float(fs)
    rms = np.sqrt(np.mean((x[:n * fs].astype(np.float64).reshape(n, fs)) ** 2, axis=1))
    reste = len(x) - n * fs
    return float(np.sum(rms > 0.05 * max(1.0, rms.max()))) + (reste / float(fs) if reste > fs // 4 else 0.0)


def bornes_sonores(x, fs=16000):
    """(debut, fin) en s sans le silence du debut et de la fin (fenetres de 0,1 s)"""
    w = fs // 10
    n = len(x) // w
    if n < 2:
        return 0.0, len(x) / float(fs)
    rms = np.sqrt(np.mean((x[:n * w].astype(np.float64).reshape(n, w)) ** 2, axis=1))
    fort = np.nonzero(rms > 0.05 * max(1.0, rms.max()))[0]
    if len(fort) == 0:
        return 0.0, len(x) / float(fs)
    return max(0.0, (fort[0] - 1) * 0.1), min(len(x) / float(fs), (fort[-1] + 2) * 0.1)


def index_base(entrees, pubs):
    """index code -> [(bloc, instant)] des pubs de la base (copie : la base peut changer pendant l'analyse)"""
    index = {}
    for hh, b, tt in entrees:
        if b in pubs:
            index.setdefault(hh & MASQUE, []).append((b, tt))
    return index


def comparer_base(index, pubs, h, t, duree_s):
    """la captation (codes h, t ; duree en s) est-elle une pub de la base ?
    -> None, ou (numero, votes, couverture de la captation, couverture de la pub, sur, debut dans la captation en s)
    sur = meme pub, meme decalage, concordances etalees sur >= 75 % de la captation et >= 75 % de la pub :
    une captation qui contient la pub connue PLUS autre chose n'est donc pas « sure »."""
    garde = (h & REQUETE) == 0
    h, t = h[garde].tolist(), t[garde].tolist()
    votes = {}
    for hh, tc in zip(h, t):
        for b, tb in index.get(hh & MASQUE, ())[:24]:
            d = tc - tb
            for cle in {(b, d >> 1), (b, (d + 1) >> 1)}:
                votes[cle] = votes.get(cle, 0) + 1
    if not votes:
        return None
    (b, dd), n = max(votes.items(), key=lambda kv: kv[1])
    if n < 10:
        return None
    # secondes de la captation ou cette pub concorde, au bon decalage (2 concordances au moins par seconde)
    par_s = {}
    for hh, tc in zip(h, t):
        for bb, tb in index.get(hh & MASQUE, ())[:24]:
            if bb == b and abs((tc - tb) - 2 * dd) <= 2:
                k = int(tc / IPS)
                par_s[k] = par_s.get(k, 0) + 1
    sec = sum(1 for v in par_s.values() if v >= 2)
    numero, duree_pub = pubs[b]
    cf = min(1.0, sec / max(1.0, duree_s))
    cp = min(1.0, sec / max(1.0, float(duree_pub)))
    debut = max(0.0, 2 * dd / IPS)                 # ou la pub commence dans la captation (s)
    return numero, n, cf, cp, (n >= SUR_VOTES and cf >= SUR_COUVERTURE and cp >= SUR_COUVERTURE), debut


# ---------------------------------------------------------------- fenetre « Captation » : decouper un long enregistrement en pubs
AIDE_CAPTATION = """Espace\t\tLire / pause (reprend où on s'est arrêté)
← / →\t\tReculer / avancer de 2 s
D / F\t\tDébut / fin de la pub, à l'endroit écouté
A\t\tAjouter le passage comme pub
+ / −\t\tZoom avant / arrière (molette aussi)
Clic\t\tAller à cet endroit ; glisser le trait jaune / rouge = début / fin

Méthode : zoomer sur un écran de pub, Lire, D au vrai début de la première pub,
F à sa fin, vérifier les coupures (« ▶ au début », « ▶ à la fin »), « Ajouter comme pub »,
puis la suivante (le début se place tout seul à la fin de la précédente).
Les empreintes sont calculées comme sur la carte ; la pub est écrite avec « Enregistrer »
dans la fenêtre principale."""


class FenetreCaptation:
    """forme du son d'une captation (ou d'un WAV importe), D / F, « Ajouter comme pub »"""
    MIN_S, MAX_S = 5.0, 420.0                      # durees d'une pub acceptees par la carte (BLOC_MIN / BLOC_MAX)

    def __init__(self, app, cap, chemins=None):
        self.app, self.cap = app, cap
        # liste des fichiers a traiter (ouverture multiple) : {'chemin', 'etat', 'nb'}
        chemins = list(chemins) if chemins else [cap.chemin]
        self.fichiers = [{'chemin': c, 'etat': 'à traiter', 'nb': 0, 'duree': None, 'codes': None, 'base': None, 'utile': None}
                         for c in chemins]
        self.gen_analyse = 0                       # v0.9.56 : analyse « deja dans la base ? » en arriere-plan
        self.cur = next((i for i, f in enumerate(self.fichiers) if f['chemin'] == cap.chemin), 0)
        self.fichiers[self.cur]['duree'] = cap.duree
        tk, ttk = app.tk, app.ttk
        self.tk, self.ttk = tk, ttk
        self.w = tk.Toplevel(app.r)
        self.w.title("Captation — " + os.path.basename(cap.chemin))
        self.w.geometry("1240x680")
        self.w.minsize(900, 560)
        self.vue = [0.0, min(cap.duree, 90.0)]
        self.sel = self.sel_defaut(cap)
        self.tete = 0.0
        self.gain = gain_auto_np(cap.x) if app.var_vol.get() else 1.0
        self.lecteur = Lecteur()
        self.jeu_t0 = None
        self.jeu_gen = 0
        self.jeu_debut = self.jeu_duree = 0.0
        self.tire = None
        self.ajoutees = []                          # (debut, fin, numero)
        self.var_nom = tk.StringVar()
        self.var_chaine = tk.StringVar(value=CHAINES[cap.chaine])
        self.var_deb, self.var_fin = tk.DoubleVar(value=self.sel[0]), tk.DoubleVar(value=self.sel[1])
        self.construire()
        self.w.bind('<Destroy>', self.fermee)
        self.w.after(100, self.dessiner)
        self.w.after(300, self.analyser)

    @classmethod
    def sel_defaut(cls, cap):
        """v0.9.54 : un morceau AUTO (pub_….wav) EST la pub : tout le fichier est choisi d'avance"""
        if os.path.basename(cap.chemin).lower().startswith('pub_') and cls.MIN_S <= cap.duree <= cls.MAX_S:
            return [0.0, round(cap.duree, 1)]
        return [0.0, round(min(cap.duree, 30.0), 1)]

    def fermee(self, e):
        if e.widget is not self.w:
            return
        self.gen_analyse += 1                      # arrete l'analyse en cours
        self.lecteur.arreter()
        n = sum(f['nb'] for f in self.fichiers)
        if n and self.app.bib and self.app.bib.modifie:
            self.app.dire("%d pub(s) ajoutée(s) depuis les captations : « Enregistrer et préparer l'envoi », "
                          "puis sur le portail « Vider » le groupe des captations traitées." % n)

    # ---------------- fenetre
    def construire(self):
        tk, ttk, w = self.tk, self.ttk, self.w
        c = self.cap
        info = "%s  ·  %s  ·  %s" % (os.path.basename(c.chemin), self.app.fmt_fin(c.duree),
                                       "16 kHz" if c.fs_origine == 16000 else "%d Hz → 16 kHz" % c.fs_origine)
        if c.chaine:
            info += "  ·  chaîne notée par la carte : " + CHAINES[c.chaine]
        if c.remarque:
            info += "  ·  " + c.remarque
        self.lbl_info = ttk.Label(w, text=info, style='Sous.TLabel')
        self.lbl_info.pack(anchor='w', padx=14, pady=(10, 4))
        self.cv = tk.Canvas(w, height=200, bg=self.app.couleurs()['fond'], highlightthickness=0)
        self.cv.pack(fill='x', padx=14)
        self.sb = ttk.Scrollbar(w, orient='horizontal', command=self.defiler)
        self.sb.pack(fill='x', padx=14)
        self.cv.bind('<Button-1>', self.appui)
        self.cv.bind('<B1-Motion>', self.glisse)
        self.cv.bind('<ButtonRelease-1>', lambda e: setattr(self, 'tire', None))
        self.cv.bind('<Motion>', self.survol)
        self.cv.bind('<Configure>', lambda e: self.dessiner())
        self.cv.bind('<MouseWheel>', lambda e: self.zoomer(0.7 if e.delta > 0 else 1 / 0.7, self.t_de_x(e.x)))
        self.cv.bind('<Button-4>', lambda e: self.zoomer(0.7, self.t_de_x(e.x)))
        self.cv.bind('<Button-5>', lambda e: self.zoomer(1 / 0.7, self.t_de_x(e.x)))

        lz = ttk.Frame(w)
        lz.pack(fill='x', padx=14, pady=(6, 0))
        ttk.Button(lz, text="Zoom −", width=8, command=lambda: self.zoomer(1 / 0.5)).pack(side='left')
        ttk.Button(lz, text="Zoom +", width=8, command=lambda: self.zoomer(0.5)).pack(side='left', padx=4)
        ttk.Button(lz, text="Tout voir", command=lambda: self.voir(0.0, self.cap.duree)).pack(side='left')
        self.lbl_vue = ttk.Label(lz, text="", style='Sous.TLabel')
        self.lbl_vue.pack(side='left', padx=10)
        self.lbl_temps = ttk.Label(lz, text="", font=('Consolas', 11))
        self.lbl_temps.pack(side='right')

        lj = ttk.Frame(w)
        lj.pack(fill='x', padx=14, pady=(8, 0))
        self.btn_lire = ttk.Button(lj, text="▶  Lire", width=10, command=self.basculer,
                                   style='Accent.TButton' if self.app.sv else 'TButton')
        self.btn_lire.pack(side='left')
        ttk.Button(lj, text="■", width=3, command=self.stop_debut).pack(side='left', padx=4)
        ttk.Button(lj, text="▶  Passage choisi", command=lambda: self.jouer(self.sel[0], self.sel[1])).pack(side='left', padx=(4, 0))
        ttk.Label(lj, text="Vérifier la coupure :", style='Sous.TLabel').pack(side='left', padx=(18, 4))
        ttk.Button(lj, text="▶ au début", command=lambda: self.verifier(0)).pack(side='left')
        ttk.Button(lj, text="▶ à la fin", command=lambda: self.verifier(1)).pack(side='left', padx=4)

        lm = ttk.Frame(w)
        lm.pack(fill='x', padx=14, pady=(8, 0))
        ttk.Button(lm, text="Début ici (D)", command=lambda: self.marque(0)).pack(side='left')
        ttk.Button(lm, text="Fin ici (F)", command=lambda: self.marque(1)).pack(side='left', padx=6)
        ttk.Label(lm, text="  Garder de").pack(side='left')
        self.sp_deb = ttk.Spinbox(lm, from_=0, to=99999, increment=0.1, textvariable=self.var_deb, width=8, command=self.lire_sel)
        self.sp_deb.pack(side='left', padx=6)
        ttk.Label(lm, text="à").pack(side='left')
        self.sp_fin = ttk.Spinbox(lm, from_=0, to=99999, increment=0.1, textvariable=self.var_fin, width=8, command=self.lire_sel)
        self.sp_fin.pack(side='left', padx=6)
        ttk.Label(lm, text="s").pack(side='left')
        self.lbl_duree = ttk.Label(lm, text="", style='Sous.TLabel')
        self.lbl_duree.pack(side='left', padx=10)
        for s in (self.sp_deb, self.sp_fin):
            s.bind('<FocusOut>', lambda e: self.lire_sel())
            s.bind('<Return>', lambda e: (self.lire_sel(), 'break')[1])

        la = ttk.LabelFrame(w, text=" Nouvelle pub ", padding=10)
        la.pack(fill='x', padx=14, pady=(10, 0))
        ttk.Label(la, text="Nom").grid(row=0, column=0, sticky='w')
        self.ent_nom = ttk.Entry(la, textvariable=self.var_nom)
        self.ent_nom.grid(row=0, column=1, sticky='we', padx=8)
        ttk.Label(la, text="Chaîne").grid(row=0, column=2, sticky='w')
        self.cmb = ttk.Combobox(la, textvariable=self.var_chaine, values=CHAINES, state='readonly', width=22)
        self.cmb.grid(row=0, column=3, padx=8)
        ttk.Button(la, text="➕  Ajouter comme pub (A)", command=self.ajouter,
                   style='Accent.TButton' if self.app.sv else 'TButton').grid(row=0, column=4, padx=(8, 0))
        ttk.Button(la, text="🔍  Tester la reconnaissance", command=self.tester).grid(row=0, column=5, padx=(8, 0))
        ttk.Button(la, text="🗑  Pas une pub : supprimer", command=self.supprimer_captation).grid(row=0, column=6, padx=(8, 0))
        la.columnconfigure(1, weight=1)
        ttk.Label(la, text="Nom vide = « Pub n°N ». La pub est ajoutée à la bibliothèque ouverte ; "
                  "à la fin, « ✅ » dans la fenêtre principale l'enregistre.", style='Sous.TLabel').grid(row=1, column=0, columnspan=5, sticky='w', pady=(6, 0))

        g = ttk.Frame(w)
        g.pack(fill='both', expand=True, padx=14, pady=(10, 4))
        # fichiers captes a corriger un a un (clic = on charge ce fichier dans cette fenetre)
        gf = ttk.Frame(g)
        gf.pack(side='left', fill='y', padx=(0, 10))
        self.btn_connues = ttk.Button(gf, text="🔎  Recherche des pubs déjà dans la base…", state='disabled',
                                      command=self.supprimer_connues)
        self.btn_connues.pack(side='top', fill='x', pady=(0, 6))
        self.fich = ttk.Treeview(gf, columns=('fichier', 'chaine', 'duree', 'etat', 'base'), show='headings', height=5, selectmode='extended')
        self.fich.tag_configure('connue', foreground='#2e9d4f')
        self.fich.tag_configure('proche', foreground='#d9822b')
        self.fich.tag_configure('fini', foreground='#8a8a8a')
        self.tri_fich = None                        # v0.9.56 : (colonne, inverse) ; clic sur un titre = trier
        for k, t, lg in (('fichier', "Fichiers captés", 130), ('chaine', "Chaîne", 90), ('duree', "Durée", 55), ('etat', "État", 90),
                         ('base', "Dans la base ?", 175)):
            self.fich.heading(k, text=t, command=lambda k=k: self.trier_fichiers(k))
            self.fich.column(k, width=lg, anchor='w' if k != 'duree' else 'center')
        sbf = ttk.Scrollbar(gf, orient='vertical', command=self.fich.yview)
        self.fich.configure(yscrollcommand=sbf.set)
        self.fich.pack(side='left', fill='y')
        sbf.pack(side='left', fill='y')
        gf.pack_configure(fill='y')
        self.fich.bind('<<TreeviewSelect>>', self.choisir_fichier)
        self.fich.bind('<Button-3>', self.menu_fichiers)
        self.fich.bind('<Double-1>', lambda e: self.comparer_choisi())
        self.fich.bind('<Delete>', lambda e: (self.supprimer_choisis(), 'break')[1])
        self.fich.bind('<Control-a>', lambda e: (self.choisir_tous(None), 'break')[1])
        m = self.menu_fich = tk.Menu(w, tearoff=0)
        m.add_command(label="▶  Ouvrir ce fichier", command=self.ouvrir_choisi)
        m.add_command(label="🎧  Comparer à l'oreille avec la pub de la base", command=self.comparer_choisi)
        m.add_separator()
        m.add_command(label="🗑  Effacer les fichiers choisis (la base n'est pas touchée)", command=self.supprimer_choisis)
        m.add_command(label="➕  Ajouter les fichiers choisis comme pubs (fichier entier)", command=self.ajouter_choisis)
        m.add_separator()
        m.add_command(label="Choisir tous les « ✓ oui, déjà dans la base »", command=lambda: self.choisir_tous('oui'))
        m.add_command(label="Choisir tous les « ? ressemble »", command=lambda: self.choisir_tous('proche'))
        m.add_command(label="Choisir tous les « nouvelle »", command=lambda: self.choisir_tous('nouvelle'))
        m.add_command(label="Tout choisir (Ctrl+A)", command=lambda: self.choisir_tous(None))
        m.add_separator()
        m.add_command(label="Trier par « Dans la base ? »", command=lambda: self.trier_fichiers('base', False))
        m.add_command(label="Trier par durée", command=lambda: self.trier_fichiers('duree', False))
        m.add_command(label="Trier par chaîne", command=lambda: self.trier_fichiers('chaine', False))
        m.add_command(label="Trier par nom (ordre de captation)", command=lambda: self.trier_fichiers('fichier', False))
        self.remplir_fichiers()
        self.liste = ttk.Treeview(g, columns=('num', 'nom', 'chaine', 'deb', 'fin', 'duree'), show='headings', height=5)
        for k, t, lg in (('num', "N°", 50), ('nom', "Pub ajoutée", 220), ('chaine', "Chaîne", 100), ('deb', "Début", 90), ('fin', "Fin", 90), ('duree', "Durée", 80)):
            self.liste.heading(k, text=t)
            self.liste.column(k, width=lg, anchor='w' if k == 'nom' else 'center', stretch=k == 'nom')
        self.liste.pack(side='left', fill='both', expand=True)
        self.lbl_statut = ttk.Label(w, text="Zoome, écoute, puis D et F aux limites de la pub (aide : touche F1).", style='Statut.TLabel')
        self.lbl_statut.pack(fill='x', side='bottom')

        def hors_saisie(f):
            def g(ev):
                if w.focus_get() in (self.ent_nom, self.sp_deb, self.sp_fin, self.cmb):
                    return None
                f()
                return 'break'
            return g
        w.bind('<space>', hors_saisie(self.basculer))
        w.bind('<KeyPress-d>', hors_saisie(lambda: self.marque(0)))
        w.bind('<KeyPress-f>', hors_saisie(lambda: self.marque(1)))
        w.bind('<KeyPress-a>', hors_saisie(self.ajouter))
        w.bind('<Left>', hors_saisie(lambda: self.deplacer(-2)))
        w.bind('<Right>', hors_saisie(lambda: self.deplacer(2)))
        w.bind('<KeyPress-plus>', hors_saisie(lambda: self.zoomer(0.5)))
        w.bind('<KeyPress-minus>', hors_saisie(lambda: self.zoomer(2.0)))
        w.bind('<KeyPress-KP_Add>', hors_saisie(lambda: self.zoomer(0.5)))
        w.bind('<KeyPress-KP_Subtract>', hors_saisie(lambda: self.zoomer(2.0)))
        w.bind('<Escape>', lambda e: self.stop())
        w.bind('<F1>', lambda e: self.app.mb.showinfo("Captation — raccourcis", AIDE_CAPTATION, parent=w))

    def dire(self, t):
        self.lbl_statut.configure(text=t)

    # ---------------- vue : conversions temps <-> pixels
    def largeur(self):
        l = self.cv.winfo_width()
        return l if l > 10 else 1100

    def x_de_t(self, t):
        return (t - self.vue[0]) / max(1e-6, self.vue[1] - self.vue[0]) * self.largeur()

    def t_de_x(self, x):
        l = self.largeur()
        return round(self.vue[0] + max(0, min(l, x)) / l * (self.vue[1] - self.vue[0]), 2)

    def voir(self, v0, v1):
        duree = self.cap.duree
        n = max(self.MIN_S, min(duree, v1 - v0))
        v0 = max(0.0, min(v0, duree - n))
        self.vue = [v0, v0 + n]
        self.dessiner()

    def zoomer(self, facteur, centre=None):
        v0, v1 = self.vue
        c = (v0 + v1) / 2 if centre is None else centre
        n = (v1 - v0) * facteur
        r = (c - v0) / max(1e-6, v1 - v0)
        self.voir(c - r * n, c - r * n + n)

    def defiler(self, *a):
        v0, v1 = self.vue
        n, duree = v1 - v0, self.cap.duree
        if a[0] == 'moveto':
            d = float(a[1]) * duree
        elif a[0] == 'scroll':
            d = v0 + int(a[1]) * n * (0.9 if a[2] == 'pages' else 0.1)
        else:
            return
        self.voir(d, d + n)

    # ---------------- dessin
    def dessiner(self):
        cv, c = self.cv, self.app.couleurs()
        cv.delete('all')
        l, h = self.largeur(), int(cv.cget('height'))
        v0, v1 = self.vue
        mid, amp = h / 2 - 8, h / 2 - 22
        mn, mx = self.cap.enveloppe(v0, v1, l)
        n = len(mn)
        g = self.gain
        xd, xf = self.x_de_t(self.sel[0]), self.x_de_t(self.sel[1])
        for i in range(n):
            x = i * l / n
            a = max(-1.0, mn[i] * g)
            b = min(1.0, mx[i] * g)
            cv.create_line(x, mid - b * amp, x, mid - a * amp + 1, fill=c['onde'] if xd <= x <= xf else c['hors'])
        for d, f, num in self.ajoutees:               # pubs deja ajoutees : bandes vertes
            if f > v0 and d < v1:
                x0, x1 = max(0, self.x_de_t(d)), min(l, self.x_de_t(f))
                cv.create_rectangle(x0, 0, x1, h - 16, fill='#2e9e5b', stipple='gray25', outline='')
                cv.create_text(x0 + 3, 3, text="n°%d" % num, fill='#2e9e5b', anchor='nw')
        cv.create_line(xd, 0, xd, h - 16, fill=c['debut'], width=2)
        cv.create_line(xf, 0, xf, h - 16, fill=c['fin'], width=2)
        pas = next((p for p in (1, 2, 5, 10, 15, 30, 60, 120, 300, 600) if (p / max(1e-6, v1 - v0)) * l >= 70), 600)
        t = int(v0 // pas) * pas
        while t <= v1:
            if t >= v0:
                x = self.x_de_t(t)
                cv.create_line(x, h - 16, x, h - 11, fill=c['texte'])
                cv.create_text(x + 2, h - 2, text=self.app.fmt_fin(t), fill=c['texte'], anchor='sw')
            t += pas
        self.curseur(self.position() if (self.jeu_t0 is not None or self.tete) else None)
        self.lbl_vue.configure(text="Vue : %s → %s (%.0f s)" % (self.app.fmt_fin(v0), self.app.fmt_fin(v1), v1 - v0))
        self.sb.set(v0 / self.cap.duree, v1 / self.cap.duree)
        self.maj_temps()
        self.maj_duree()

    def curseur(self, t):
        self.cv.delete('curseur')
        if t is None or not (self.vue[0] <= t <= self.vue[1]):
            return
        x = self.x_de_t(t)
        self.cv.create_line(x, 0, x, int(self.cv.cget('height')) - 16, fill=self.app.couleurs()['curseur'], tags='curseur')

    def maj_duree(self):
        d = self.sel[1] - self.sel[0]
        ok = self.MIN_S <= d <= self.MAX_S
        self.lbl_duree.configure(text="durée %s%s" % (self.app.fmt_fin(d), "" if ok else
                                                      ("  (trop courte : 5 s au moins)" if d < self.MIN_S else "  (trop longue : 7 min au plus)")))

    def maj_temps(self):
        self.lbl_temps.configure(text="%s / %s" % (self.app.fmt_fin(self.position()), self.app.fmt_fin(self.cap.duree)))

    # ---------------- souris
    def pres_marque(self, x):
        for k in (0, 1):
            if abs(self.x_de_t(self.sel[k]) - x) <= 8:
                return k
        return None

    def appui(self, ev):
        self.tire = self.pres_marque(ev.x)
        if self.tire is None:
            t = self.t_de_x(ev.x)
            if self.jeu_t0 is not None:
                self.jouer(t, self.cap.duree)
            else:
                self.tete = t
                self.curseur(t)
                self.maj_temps()

    def glisse(self, ev):
        if self.tire is not None:
            self.poser(self.tire, self.t_de_x(ev.x))

    def survol(self, ev):
        self.cv.configure(cursor='sb_h_double_arrow' if self.pres_marque(ev.x) is not None else '')

    # ---------------- marques
    def poser(self, quoi, t):
        t = round(max(0.0, min(self.cap.duree, t)), 1)
        t = min(t, self.sel[1]) if quoi == 0 else max(t, self.sel[0])
        self.sel[quoi] = t
        (self.var_deb if quoi == 0 else self.var_fin).set(t)
        self.dessiner()

    def lire_sel(self):
        try:
            d, f = float(self.var_deb.get()), float(self.var_fin.get())
        except (self.tk.TclError, ValueError):
            return
        d = max(0.0, min(d, self.cap.duree))
        f = max(d, min(f, self.cap.duree))
        self.sel = [d, f]
        self.dessiner()

    def marque(self, quoi):
        t = round(self.position(), 1)
        if quoi == 0 and t > self.sel[1]:                # un debut apres l'ancienne fin : la fin suit
            self.sel[1] = min(self.cap.duree, t + 30.0)
            self.var_fin.set(round(self.sel[1], 1))
        self.poser(quoi, t)
        self.dire("%s de la pub : %s" % ("Début" if quoi == 0 else "Fin", self.app.fmt_fin(t)))

    def verifier(self, quoi):
        d, f = self.sel
        if quoi == 0:
            self.jouer(d, d + 3.0)
        else:
            self.jouer(max(d, f - 3.0), f)

    # ---------------- lecture (par morceaux de 5 min au plus)
    def position(self):
        if self.jeu_t0 is not None:
            return min(self.jeu_debut + time.time() - self.jeu_t0, self.jeu_debut + self.jeu_duree)
        return self.tete

    def jouer(self, d, f):
        fs = Captation.FS
        d = max(0.0, min(d, self.cap.duree))
        f = max(d, min(f, self.cap.duree, d + 300.0))
        bout = self.cap.x[int(d * fs):int(f * fs)]
        if len(bout) < fs // 10:
            return
        if self.gain != 1.0:
            bout = np.clip(bout.astype(np.float32) * self.gain, -32768, 32767).astype(np.int16)
        self.lecteur.jouer(bout, fs)
        self.jeu_gen += 1
        self.jeu_t0, self.jeu_debut, self.jeu_duree = time.time(), d, len(bout) / float(fs)
        self.btn_lire.configure(text="❚❚  Pause")
        self.avancer(self.jeu_gen)

    def avancer(self, gen):
        if self.jeu_t0 is None or gen != self.jeu_gen:
            return
        e = time.time() - self.jeu_t0
        if e > self.jeu_duree:
            fin = self.jeu_debut + self.jeu_duree
            self.jeu_t0 = None
            self.tete = 0.0 if fin >= self.cap.duree - 0.05 else fin
            self.btn_lire.configure(text="▶  Lire")
            self.curseur(self.tete if self.tete else None)
            self.maj_temps()
            return
        t = self.jeu_debut + e
        if not (self.vue[0] <= t <= self.vue[1]):          # la lecture sort de la vue : elle suit
            self.voir(t - 5, t - 5 + (self.vue[1] - self.vue[0]))
        self.curseur(t)
        self.maj_temps()
        self.w.after(40, lambda: self.avancer(gen))

    def stop(self):
        if self.jeu_t0 is not None:
            self.tete = self.position()
        self.lecteur.arreter()
        self.jeu_t0 = None
        self.btn_lire.configure(text="▶  Lire")

    def stop_debut(self):
        self.stop()
        self.tete = 0.0
        self.curseur(None)
        self.maj_temps()

    def basculer(self):
        if self.jeu_t0 is not None:
            self.stop()
            self.curseur(self.tete)
            self.maj_temps()
        else:
            self.jouer(self.tete if self.tete < self.cap.duree - 0.2 else 0.0, self.cap.duree)

    def deplacer(self, ds):
        en_cours = self.jeu_t0 is not None
        t = max(0.0, min(self.cap.duree, self.position() + ds))
        self.stop()
        self.tete = t
        self.maj_temps()
        self.curseur(t)
        if en_cours:
            self.jouer(t, self.cap.duree)

    # ---------------- ajouter la pub a la bibliotheque
    def progres(self, p):
        self.dire("Calcul des empreintes de la captation (une seule fois)… %d %%" % int(p * 100))
        self.w.update_idletasks()

    # ---------------- liste des fichiers captes (ouverture multiple)
    def remplir_fichiers(self):
        self.fich.delete(*self.fich.get_children())
        for i in self.ordre_fichiers():
            f = self.fichiers[i]
            d = self.app.fmt_fin(f['duree']) if f['duree'] is not None else "—"
            ch = chaine_captation(f['chemin'])
            self.fich.insert('', 'end', iid=str(i), values=(os.path.basename(f['chemin']), CHAINES[ch] if ch else "—", d, f['etat'],
                                                            self.texte_base(f)), tags=self.tag_base(f))
        self.fich.selection_set(str(self.cur))

    # ---------------- v0.9.56 : captations deja dans la base
    @staticmethod
    def texte_base(f):
        r = f['base']
        if r is None:
            return "…" if f['etat'] == 'à traiter' else ""
        if r == 'nouvelle':
            return "non, nouvelle"
        num, _, cf, cp, sur = r[:5]
        return ("✓ oui : pub n°%d" % num) if sur else ("? ressemble à n°%d (%d %%)" % (num, 100 * min(cf, cp)))

    @staticmethod
    def tag_base(f):
        r = f['base']
        if f['etat'] != 'à traiter':
            return ('fini',)
        if r is None or r == 'nouvelle':
            return ()
        return ('connue',) if r[4] else ('proche',)

    def connue(self, f):
        return f['etat'] == 'à traiter' and f['base'] not in (None, 'nouvelle') and f['base'][4]

    def analyser(self):
        """compare chaque fichier « à traiter » a la base, dans un fil a part (la fenetre reste utilisable)"""
        import threading
        bib = self.app.bib
        if not bib or not bib.pubs:
            self.btn_connues.configure(text="Pas de base ouverte : rien à comparer")
            return
        self.gen_analyse += 1
        gen = self.gen_analyse
        pubs = {i: (p.numero, p.duree_s) for i, p in bib.pubs.items() if p.type == PUB}
        entrees = list(bib.entrees)
        a_voir = [i for i, f in enumerate(self.fichiers) if f['etat'] == 'à traiter']
        for i in a_voir:
            self.fichiers[i]['base'] = None
        self.resultats = []
        self.btn_connues.configure(text="🔎  Recherche des pubs déjà dans la base…", state='disabled')

        def fil():
            index = index_base(entrees, pubs)
            for i in a_voir:
                if gen != self.gen_analyse:
                    return
                f = self.fichiers[i]
                try:
                    if f['codes'] is None:
                        c = Captation(f['chemin'])
                        f['codes'], f['duree'], f['utile'] = c.codes(), c.duree, secondes_sonores(c.x)
                    h, t = f['codes']
                    r = comparer_base(index, pubs, h, t, f['utile'])
                except (OSError, ValueError, RuntimeError):
                    r = 'illisible'
                self.resultats.append((gen, i, r if r is not None else 'nouvelle'))
            self.resultats.append((gen, None, None))
        threading.Thread(target=fil, daemon=True).start()
        self.w.after(200, self.suivre_analyse)

    def suivre_analyse(self):
        try:
            if not self.w.winfo_exists():
                return
        except Exception:
            return
        fini = False
        while self.resultats:
            gen, i, r = self.resultats.pop(0)
            if gen != self.gen_analyse:
                continue
            if i is None:
                fini = True
                continue
            f = self.fichiers[i]
            if r == 'illisible':
                f['base'] = None
            else:
                f['base'] = r
            if self.fich.exists(str(i)):
                self.fich.item(str(i), tags=self.tag_base(f))
                self.fich.set(str(i), 'base', self.texte_base(f) if r != 'illisible' else "illisible")
                if f['duree'] is not None:
                    self.fich.set(str(i), 'duree', self.app.fmt_fin(f['duree']))
        n = sum(1 for f in self.fichiers if self.connue(f))
        if fini:
            self.btn_connues.configure(text=("🗑  Effacer les %d enregistrements déjà dans la base (la base n'est pas touchée)" % n) if n
                                       else "Aucun enregistrement n'est déjà dans la base", state='normal' if n else 'disabled')
            proches = sum(1 for f in self.fichiers if f['etat'] == 'à traiter' and f['base'] not in (None, 'nouvelle') and not f['base'][4])
            self.dire("Comparaison finie : %d déjà dans la base (sûr)%s." % (
                n, (", %d qui ressemblent (à écouter)" % proches) if proches else ""))
        else:
            self.btn_connues.configure(text="🔎  Recherche… %d déjà dans la base" % n)
            self.w.after(200, self.suivre_analyse)

    def supprimer_connues(self):
        app = self.app
        liste = [i for i, f in enumerate(self.fichiers) if self.connue(f)]
        if not liste:
            return
        apercu = "\n".join("  • %s  →  pub n°%d" % (os.path.basename(self.fichiers[i]['chemin']), self.fichiers[i]['base'][0])
                           for i in liste[:12]) + ("\n  • …" if len(liste) > 12 else "")
        if not app.mb.askyesno("Supprimer les captations déjà dans la base",
                               "Ces %d captations sont des pubs déjà dans la base (même pub sur au moins %d %% de sa durée) :\n\n%s\n\n"
                               "Supprimer ces fichiers ? La base n'est pas touchée."
                               % (len(liste), int(SUR_COUVERTURE * 100), apercu), parent=self.w):
            return
        self.stop()
        fait = 0
        for i in liste:
            try:
                os.remove(self.fichiers[i]['chemin'])
            except OSError:
                continue
            self.fichiers[i]['etat'] = 'supprimée'
            fait += 1
        self.remplir_fichiers()
        app.dire("%d captation(s) déjà dans la base supprimée(s)." % fait)
        self.btn_connues.configure(text="Aucun enregistrement n'est déjà dans la base", state='disabled')
        if self.fichiers[self.cur]['etat'] == 'supprimée':
            suiv = self.suivant_a_traiter()
            if suiv is None:
                self.w.destroy()
                return
            self.charger(suiv)
        else:
            self.dire("%d supprimée(s). Il reste les captations à regarder." % fait)

    def choisir_fichier(self, _=None):
        sel = self.fich.selection()
        if len(sel) != 1:                          # v0.9.56 : selection multiple = on ne charge rien
            n = len(sel)
            if n > 1:
                self.dire("%d fichiers choisis : clic droit pour une action sur tous." % n)
            return
        if sel and int(sel[0]) != self.cur and self.fichiers[int(sel[0])]['etat'] != 'supprimée':
            self.charger(int(sel[0]))
        elif sel and int(sel[0]) != self.cur:
            self.fich.selection_set(str(self.cur))

    # ---------------- v0.9.56 : tri, selection multiple, actions groupees (clic droit)
    def categorie(self, f):
        """0 deja dans la base (sur), 1 ressemble, 2 nouvelle, 3 en cours d'analyse, 4 traite / supprime"""
        if f['etat'] != 'à traiter':
            return 4
        r = f['base']
        if r is None:
            return 3
        if r == 'nouvelle':
            return 2
        return 0 if r[4] else 1

    def ordre_fichiers(self):
        idx = list(range(len(self.fichiers)))
        if not self.tri_fich:
            return idx
        col, inv = self.tri_fich
        F = self.fichiers
        cle = {'fichier': lambda i: os.path.basename(F[i]['chemin']).lower(),
               'chaine': lambda i: (CHAINES[chaine_captation(F[i]['chemin'])] if chaine_captation(F[i]['chemin']) else "~"),
               'duree': lambda i: F[i]['duree'] if F[i]['duree'] is not None else 1e9,
               'etat': lambda i: F[i]['etat'],
               'base': lambda i: (self.categorie(F[i]), F[i]['base'][0] if isinstance(F[i]['base'], tuple) else 0,
                                  os.path.basename(F[i]['chemin']))}[col]
        return sorted(idx, key=cle, reverse=inv)

    def trier_fichiers(self, col, inverse=None):
        if inverse is None:                        # clic sur le titre : 2e clic = ordre inverse
            inverse = bool(self.tri_fich and self.tri_fich[0] == col and not self.tri_fich[1])
        self.tri_fich = (col, inverse)
        sel = self.fich.selection()
        self.remplir_fichiers()
        if sel:
            self.fich.selection_set([i for i in sel if self.fich.exists(i)])

    def choisis(self):
        return [int(i) for i in self.fich.selection()]

    def menu_fichiers(self, ev):
        iid = self.fich.identify_row(ev.y)
        if iid and iid not in self.fich.selection():
            self.fich.selection_set(iid)
        n = len(self.fich.selection())
        self.menu_fich.entryconfigure(0, state='normal' if n == 1 else 'disabled')
        c = self.choisis()
        self.menu_fich.entryconfigure(1, state='normal' if n == 1 and isinstance(self.fichiers[c[0]]['base'], tuple) else 'disabled')
        self.menu_fich.entryconfigure(3, label="🗑  Effacer les %d fichier(s) choisi(s) (la base n'est pas touchée)" % n,
                                      state='normal' if n else 'disabled')
        self.menu_fich.entryconfigure(4, label="➕  Ajouter les %d fichier(s) choisi(s) comme pubs (fichier entier)" % n,
                                      state='normal' if n else 'disabled')
        self.menu_fich.tk_popup(ev.x_root, ev.y_root)

    def comparer_choisi(self):
        c = self.choisis()
        if len(c) != 1:
            return
        f = self.fichiers[c[0]]
        r = f['base']
        if not isinstance(r, tuple):
            self.dire("Rien à comparer : ce fichier ne ressemble à aucune pub de la base.")
            return
        pub = next((p for p in self.app.bib.pubs.values() if p.numero == r[0]), None)
        if pub is None:
            self.dire("La pub n°%d n'est plus dans la base." % r[0])
            return
        self.stop()
        self.comparateur = FenetreComparer(self, c[0], pub, r)

    def ouvrir_choisi(self):
        c = self.choisis()
        if len(c) == 1 and c[0] != self.cur and self.fichiers[c[0]]['etat'] != 'supprimée':
            self.charger(c[0])

    def choisir_tous(self, quoi):
        cat = {'oui': 0, 'proche': 1, 'nouvelle': 2}.get(quoi)
        ids = [str(i) for i, f in enumerate(self.fichiers)
               if self.fich.exists(str(i)) and f['etat'] == 'à traiter' and (cat is None or self.categorie(f) == cat)]
        self.fich.selection_set(ids)
        if ids:
            self.fich.see(ids[0])
        self.dire("%d fichier(s) choisi(s)." % len(ids) + (" Clic droit pour une action sur tous." if ids else ""))

    def apres_lot(self):
        """apres une action groupee : liste a jour, et le fichier ouvert s'il a disparu"""
        self.remplir_fichiers()
        n = sum(1 for f in self.fichiers if self.connue(f))
        self.btn_connues.configure(text=("🗑  Effacer les %d enregistrements déjà dans la base (la base n'est pas touchée)" % n) if n
                                   else "Aucun enregistrement n'est déjà dans la base", state='normal' if n else 'disabled')
        if self.fichiers[self.cur]['etat'] in ('supprimée',) or self.fichiers[self.cur]['etat'].startswith('✓'):
            suiv = self.suivant_a_traiter()
            if suiv is None:
                self.dire("Tous les fichiers sont traités. Ferme cette fenêtre, puis « ✅ » dans la fenêtre principale.")
                return
            self.charger(suiv)

    def supprimer_choisis(self):
        app = self.app
        ids = [i for i in self.choisis() if self.fichiers[i]['etat'] not in ('supprimée',)]
        if not ids:
            return
        nb_oui = sum(1 for i in ids if self.categorie(self.fichiers[i]) == 0)
        autres = len(ids) - nb_oui
        texte = "Effacer %d fichier(s) de captation ?\n\n" % len(ids)
        texte += "  • %d déjà dans la base (sûr)\n" % nb_oui if nb_oui else ""
        texte += ("  • %d PAS déjà dans la base : leur son sera perdu s'il contenait une pub nouvelle\n" % autres) if autres else ""
        texte += "\nLa base des pubs n'est pas touchée."
        if not app.mb.askyesno("Effacer les fichiers choisis", texte, parent=self.w):
            return
        self.stop()
        fait = 0
        for i in ids:
            try:
                os.remove(self.fichiers[i]['chemin'])
            except OSError:
                continue
            self.fichiers[i]['etat'] = 'supprimée'
            fait += 1
        app.dire("%d fichier(s) de captation effacé(s) ; la base n'est pas touchée." % fait)
        self.apres_lot()

    def ajouter_choisis(self):
        """chaque fichier choisi (un morceau AUTO = une pub) est ajoute ENTIER comme pub ; ceux deja dans la base sont sautes"""
        app, bib = self.app, self.app.bib
        if not bib:
            app.mb.showwarning("Ajouter", "Ouvre d'abord la base (fenêtre principale).", parent=self.w)
            return
        ids = [i for i in self.choisis() if self.fichiers[i]['etat'] == 'à traiter']
        deja = [i for i in ids if self.categorie(self.fichiers[i]) == 0]
        a_faire = [i for i in ids if i not in deja]
        if not a_faire:
            app.mb.showinfo("Ajouter", "Rien à ajouter : les fichiers choisis sont déjà dans la base ou déjà traités.", parent=self.w)
            return
        proches = sum(1 for i in a_faire if self.categorie(self.fichiers[i]) == 1)
        if not app.mb.askyesno("Ajouter comme pubs",
                               "Ajouter %d fichier(s) ENTIER(S) comme pubs ?\n\n%s%s"
                               "Chaque fichier doit être UNE pub (5 s à 7 min).\nCtrl+Z dans la fenêtre principale annule tout l'ajout."
                               % (len(a_faire), ("%d déjà dans la base : sautés.\n" % len(deja)) if deja else "",
                                  ("%d « ressemble » : vérifie qu'il ne s'agit pas d'une pub déjà connue.\n\n" % proches) if proches else "\n"),
                               parent=self.w):
            return
        self.stop()
        app.photo()
        ok, refus = 0, []
        self.w.configure(cursor='watch')
        try:
            for k, i in enumerate(a_faire):
                f = self.fichiers[i]
                self.dire("Ajout %d / %d : %s…" % (k + 1, len(a_faire), os.path.basename(f['chemin'])))
                self.w.update_idletasks()
                try:
                    cap = Captation(f['chemin'])
                    if f['codes'] is not None:
                        cap._codes = f['codes']
                    if not (self.MIN_S <= cap.duree <= self.MAX_S):
                        raise ValueError("durée %.1f s" % cap.duree)
                    h, t, t0, t1, son8 = cap.passage(0.0, cap.duree)
                    p = bib.ajouter_pub(h, t, t0, t1, son8, '', chaine_captation(f['chemin']))
                except (OSError, ValueError, RuntimeError) as e:
                    refus.append("%s : %s" % (os.path.basename(f['chemin']), e))
                    continue
                f['etat'], f['nb'] = "✓ pub n°%d" % p.numero, 1
                ok += 1
        finally:
            self.w.configure(cursor='')
        app.remplir()
        msg = "%d pub(s) ajoutée(s). « ✅ » dans la fenêtre principale pour enregistrer." % ok
        if refus:
            msg += "\n\nPas ajoutés :\n" + "\n".join(refus[:10])
        app.mb.showinfo("Ajouter comme pubs", msg, parent=self.w)
        app.dire("%d pub(s) ajoutée(s) depuis les captations." % ok)
        self.apres_lot()
        self.analyser()                            # les autres fichiers peuvent contenir ces nouvelles pubs

    def charger(self, i):
        """charge le fichier i dans cette fenetre (meme fenetre, pas une de plus)"""
        app = self.app
        self.stop()
        try:
            self.w.configure(cursor='watch')
            self.w.update_idletasks()
            cap = Captation(self.fichiers[i]['chemin'])
        except (OSError, ValueError, RuntimeError) as e:
            self.w.configure(cursor='')
            if app.proposer_suppression(self.fichiers[i]['chemin'], str(e), parent=self.w):
                self.fichiers[i]['etat'] = 'supprimée'
                self.fich.set(str(i), 'etat', 'supprimée')
                suiv = None
                n = len(self.fichiers)
                for k in range(1, n):
                    j = (i + k) % n
                    if self.fichiers[j]['etat'] not in ('supprimée', 'inutilisable'):
                        suiv = j
                        break
                if suiv is not None:
                    self.charger(suiv)
                    return
            else:
                self.fichiers[i]['etat'] = 'inutilisable'
                self.fich.set(str(i), 'etat', 'inutilisable')
            self.fich.selection_set(str(self.cur))
            return
        finally:
            self.w.configure(cursor='')
        if self.fichiers[i]['codes'] is not None:
            cap._codes = self.fichiers[i]['codes']
        self.cap, self.cur = cap, i
        self.fichiers[i]['duree'] = cap.duree
        self.w.title("Captation — " + os.path.basename(cap.chemin))
        info = "%s  ·  %s  ·  %s" % (os.path.basename(cap.chemin), app.fmt_fin(cap.duree),
                                       "16 kHz" if cap.fs_origine == 16000 else "%d Hz → 16 kHz" % cap.fs_origine)
        if cap.chaine:
            info += "  ·  chaîne notée par la carte : " + CHAINES[cap.chaine]
        if cap.remarque:
            info += "  ·  " + cap.remarque
        self.lbl_info.configure(text=info)
        self.vue = [0.0, min(cap.duree, 90.0)]
        self.sel = self.sel_defaut(cap)
        self.tete = 0.0
        self.gain = gain_auto_np(cap.x) if app.var_vol.get() else 1.0
        self.ajoutees = []
        self.liste.delete(*self.liste.get_children())
        self.var_chaine.set(CHAINES[cap.chaine])
        self.var_deb.set(self.sel[0])
        self.var_fin.set(self.sel[1])
        self.remplir_fichiers()
        self.maj_duree()
        self.dessiner()
        self.dire("Fichier %d / %d : %s" % (i + 1, len(self.fichiers), os.path.basename(cap.chemin)))

    def marquer_fichier(self, etat, nb=None):
        f = self.fichiers[self.cur]
        f['etat'] = etat
        if nb is not None:
            f['nb'] = nb
        self.fich.set(str(self.cur), 'etat', etat)

    def suivant_a_traiter(self):
        """index du prochain fichier non supprime apres le courant (puis avant), ou None"""
        n = len(self.fichiers)
        for connues in (False, True):              # v0.9.56 : d'abord celles qui ne sont pas deja dans la base
            for k in range(1, n):
                j = (self.cur + k) % n
                if self.fichiers[j]['etat'] not in ('supprimée', 'inutilisable') and (connues or not self.connue(self.fichiers[j])):
                    return j
        return None

    def supprimer_captation(self):
        """captation nulle ou pas une pub : le fichier est supprime (v0.9.54 : plus de dossier _supprimees),
        on passe au suivant"""
        app = self.app
        ch = self.cap.chemin
        if not app.mb.askyesno("Pas une pub", "Supprimer « %s » ?" % os.path.basename(ch), parent=self.w):
            return
        self.stop()
        try:
            os.remove(ch)
        except OSError as e:
            app.mb.showerror("Supprimer", "Impossible de supprimer : %s" % e, parent=self.w)
            return
        app.dire("« %s » supprimé." % os.path.basename(ch))
        self.marquer_fichier('supprimée')
        suiv = self.suivant_a_traiter()
        if suiv is None:
            self.w.destroy()
        else:
            self.charger(suiv)

    def tester(self):
        """C2 : rejoue TOUTE la captation contre la bibliotheque ouverte et liste ce que la carte reconnaitrait"""
        app, bib = self.app, self.app.bib
        if not bib or not bib.pubs:
            app.mb.showwarning("Tester", "Ouvre d'abord une bibliothèque qui contient des pubs (Fichier → Ouvrir la carte SD).", parent=self.w)
            return
        self.stop()
        try:
            self.w.configure(cursor='watch')
            self.dire("Calcul des empreintes et comparaison avec la bibliothèque…")
            self.w.update_idletasks()
            h, t = self.cap.codes(self.progres)
            trouves = bib.balayer(h, t)
        finally:
            self.w.configure(cursor='')
        self.afficher_test(trouves)

    def afficher_test(self, trouves):
        tk, ttk, app = self.tk, self.ttk, self.app
        fen = tk.Toplevel(self.w)
        fen.title("Test de reconnaissance — " + os.path.basename(self.cap.chemin))
        fen.geometry("760x520")
        lignes, fin_prec, couvert = [], 0.0, 0.0
        for debut, num, nom, votes, duree in trouves:
            fin = min(self.cap.duree, debut + duree)
            if debut - fin_prec >= self.MIN_S:
                lignes.append((fin_prec, debut, "—", "non reconnu", "", True))
            lignes.append((debut, fin, str(num), nom, str(votes), False))
            couvert += max(0.0, fin - max(debut, fin_prec))
            fin_prec = max(fin_prec, fin)
        if self.cap.duree - fin_prec >= self.MIN_S:
            lignes.append((fin_prec, self.cap.duree, "—", "non reconnu", "", True))
        nb = len(trouves)
        resume = ("%d passage%s reconnu%s · %s sur %s (%.0f %%)" % (
            nb, "s" if nb > 1 else "", "s" if nb > 1 else "", app.fmt_fin(couvert), app.fmt_fin(self.cap.duree),
            100.0 * couvert / self.cap.duree)) if nb else "Aucune pub de la bibliothèque n'est reconnue dans cette captation."
        ttk.Label(fen, text=resume, style='Sous.TLabel').pack(anchor='w', padx=14, pady=(12, 6))
        cadre = ttk.Frame(fen)
        cadre.pack(fill='both', expand=True, padx=14)
        arbre = ttk.Treeview(cadre, columns=('deb', 'fin', 'num', 'nom', 'votes'), show='headings')
        for k, titre, lg in (('deb', "Début", 80), ('fin', "Fin", 80), ('num', "N°", 50), ('nom', "Pub", 340), ('votes', "Votes", 70)):
            arbre.heading(k, text=titre)
            arbre.column(k, width=lg, anchor='w' if k == 'nom' else 'center', stretch=k == 'nom')
        arbre.tag_configure('trou', foreground='#d9822b')
        for d, f, num, nom, votes, trou in lignes:
            arbre.insert('', 'end', values=(app.fmt_fin(d), app.fmt_fin(f), num, nom, votes), tags=('trou',) if trou else ())
        arbre.pack(side='left', fill='both', expand=True)
        vs = ttk.Scrollbar(cadre, orient='vertical', command=arbre.yview)
        vs.pack(side='right', fill='y')
        arbre.configure(yscrollcommand=vs.set)

        def aller(_ev=None):
            sel = arbre.selection()
            if not sel:
                return
            i = arbre.index(sel[0])
            d, f = lignes[i][0], lignes[i][1]
            self.sel = [round(d, 1), round(f, 1)]
            self.var_deb.set(self.sel[0])
            self.var_fin.set(self.sel[1])
            self.tete = d
            self.voir(max(0.0, d - 5.0), min(self.cap.duree, f + 5.0))
            self.dessiner()
        arbre.bind('<Double-1>', aller)
        ttk.Label(fen, text="Double-clic sur une ligne : va à ce passage dans la captation. Les lignes orange sont des "
                  "morceaux que la carte ne reconnaîtrait pas : des pubs à découper et à ajouter.", style='Sous.TLabel',
                  wraplength=720).pack(anchor='w', padx=14, pady=8)
        self.dire(resume)

    def ajouter(self):
        app, bib = self.app, self.app.bib
        if not bib:
            app.mb.showwarning("Ajouter", "Ouvre d'abord la carte SD du détecteur (fenêtre principale : Fichier → Ouvrir).", parent=self.w)
            return
        self.lire_sel()
        d, f = self.sel
        if not (self.MIN_S <= f - d <= self.MAX_S):
            app.mb.showwarning("Ajouter", "Une pub doit durer entre 5 secondes et 7 minutes (là : %.1f s)." % (f - d), parent=self.w)
            return
        self.stop()
        try:
            self.w.configure(cursor='watch')
            h, t, t0, t1, son8 = self.cap.passage(d, f, self.progres)
            dans = (t >= t0) & (t <= t1) & ((h & REQUETE) == 0)
            sim = bib.similaire(h[dans], t[dans] - t0)
            if sim and not app.mb.askyesno("Ajouter", "Ce passage ressemble beaucoup à la pub n°%d (%d empreintes concordantes).\n"
                                           "L'ajouter quand même ?" % sim, parent=self.w):
                self.dire("Pas ajoutée (déjà connue : pub n°%d)." % sim[0])
                return
            try:
                ch = CHAINES.index(self.var_chaine.get())
            except ValueError:
                ch = 0
            n_pile = len(app.annuler_pile)
            app.photo()
            try:
                p = bib.ajouter_pub(h, t, t0, t1, son8, self.var_nom.get(), ch)
            except ValueError:
                del app.annuler_pile[n_pile:]                 # rien n'a change : pas de Ctrl+Z a garder
                raise
        except ValueError as e:
            app.mb.showwarning("Ajouter", str(e), parent=self.w)
            return
        finally:
            self.w.configure(cursor='')
        self.ajoutees.append((d, f, p.numero))
        self.liste.insert('', 'end', values=(p.numero, p.nom, CHAINES[ch] if 0 < ch < len(CHAINES) else "—", self.app.fmt_fin(d), self.app.fmt_fin(f), self.app.fmt(f - d)))
        self.marquer_fichier("✓ %d pub(s)" % len(self.ajoutees), len(self.ajoutees))
        self.var_nom.set('')
        self.sel = [f, round(min(self.cap.duree, f + 30.0), 1)]            # la pub suivante commence ici
        self.var_deb.set(self.sel[0])
        self.var_fin.set(self.sel[1])
        app.remplir()
        app.dire("Pub n°%d ajoutée (%d empreintes). « Enregistrer et préparer l'envoi » quand tu as fini." % (p.numero, p.nb))
        self.dire("Pub n°%d ajoutée : %s → %s." % (p.numero, self.app.fmt_fin(d), self.app.fmt_fin(f)))
        self.dessiner()
        # v0.9.54 : un morceau AUTO ne contient qu'une pub : on passe tout de suite au fichier suivant
        self.analyser()                              # v0.9.56 : la nouvelle pub peut etre dans d'autres fichiers
        if os.path.basename(self.cap.chemin).lower().startswith('pub_'):
            a_faire = lambda j: self.fichiers[j]['etat'] == 'à traiter' and not self.connue(self.fichiers[j])
            suiv = next((j for j in range(self.cur + 1, len(self.fichiers)) if a_faire(j)), None)
            if suiv is None:
                suiv = next((j for j in range(len(self.fichiers)) if a_faire(j)), None)
            if suiv is not None:
                self.w.after(400, lambda: self.charger(suiv))
            else:
                self.dire("Tous les fichiers sont traités. Ferme cette fenêtre, puis « Enregistrer et préparer l'envoi ».")


# ---------------------------------------------------------------- v0.9.55 : nettoyage d'une carte SD (branchée sur le PC)
# Principe : 1) copie complete de la carte dans DetecteurPub/sauvegardes_carte_sd (ZIP), 2) liste en couleurs,
# rien n'est efface sans confirmation, 3) seules les lignes cochees sont traitees. Un fichier ou un dossier
# inconnu n'est jamais coche d'office. La base (base.bin, infos.bin), les sons des pubs, la voix, le mot-cle,
# les certificats TV et l'historique ne sont jamais touches.
DOSSIER_COPIES_SD = os.path.join(DOSSIER_PC, 'sauvegardes_carte_sd')
COPIES_SD_GARDEES = 3
# dossiers systeme (Windows, macOS) a la racine : on n'y touche pas et on ne les montre pas
SYSTEME_SD = {'system volume information', '$recycle.bin', '.trashes', '.spotlight-v100', '.fseventsd', '.temporaryitems'}


class LigneSD:
    """une ligne de la liste de nettoyage
    cat : 'garder' (vert), 'inutile' (orange), 'captation' (bleu), 'inconnu' (gris), 'alerte' (rouge, information)
    action : None (rien), 'suppr' (effacer les chemins), 'appliquer' (x.new -> x.bin, comme la carte au demarrage),
             'transferer' (anciennes sauvegardes Python : copiees dans DetecteurPub/sauvegardes, puis effacees)"""
    def __init__(self, cat, titre, chemins=(), action=None, coche=False, detail=''):
        self.cat, self.titre, self.chemins, self.action = cat, titre, list(chemins), action
        self.coche, self.detail = coche, detail
        self.taille = sum(taille_chemin(c) for c in self.chemins)

    @property
    def modifiable(self):
        return self.action is not None

    def fait(self):
        return {'suppr': "effacer", 'appliquer': "reprendre", 'transferer': "ranger sur le PC puis effacer"}.get(self.action, "—")


def taille_chemin(c):
    if os.path.isfile(c):
        return os.path.getsize(c)
    t = 0
    for r, _, fs in os.walk(c):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(r, f))
            except OSError:
                pass
    return t


def fmt_taille(o):
    if o < 1024 * 1024:
        return "%d Ko" % max(1, (o + 1023) // 1024) if o else "0"
    return "%.1f Mo" % (o / 1048576) if o < 1024 ** 3 else "%.2f Go" % (o / 1024 ** 3)


def racine_sd(chemin):
    """racine de la carte (le dossier qui contient pub/), ou None"""
    c = os.path.abspath(chemin)
    if os.path.basename(c).lower() == 'pub' and os.path.isfile(os.path.join(c, 'base.bin')):
        c = os.path.dirname(c)
    return c if os.path.isdir(os.path.join(c, 'pub')) else None


def analyser_sd(racine):
    """classe tout ce qui est sur la carte. Renvoie (lignes, resume, bib) ; bib = None si la base est illisible.
    N'ecrit rien."""
    pub = os.path.join(racine, 'pub')
    lignes = []
    vus = set()                                       # chemins deja classes (le reste = inconnu)

    def classer(*chemins):
        for c in chemins:
            vus.add(os.path.normcase(os.path.abspath(c)))

    def p(*n):
        return os.path.join(pub, *n)

    # ---- la base : lisible et au bon format ?
    bib, resume = None, ''
    try:
        bib = Bibliotheque(pub)
        npubs = sum(1 for x in bib.pubs.values() if x.type == PUB)
        nv = sum(x.a_verifier for x in bib.pubs.values() if x.type == PUB)
        resume = "Base lisible : %d pubs, %d à vérifier · format compatible avec le détecteur v0.9.54" % (npubs, nv)
        if bib.source == 'base.new':
            resume += " (dernière sauvegarde base.new reprise)"
    except (OSError, ValueError, struct.error) as e:
        resume = "Base illisible (base.bin absent ou abîmé) : les sons des pubs ne seront pas touchés."
        lignes.append(LigneSD('alerte', "Base illisible : aucun son n'est proposé à l'effacement", detail=str(e)))

    # ---- base.new / infos.new : sauvegarde de la carte interrompue ou pas encore reprise
    for neuf, bon in (('base.new', 'base.bin'), ('infos.new', 'infos.bin')):
        c = p(neuf)
        if not os.path.isfile(c):
            continue
        classer(c)
        d = open(c, 'rb').read()
        if neuf == 'base.new':
            entier = Bibliotheque._valide(d)
        else:
            entier = len(d) == TAILLE_INFOS and struct.unpack_from('<I', d, 0)[0] == MAGIE_INFOS
        if entier and bib is not None:
            lignes.append(LigneSD('inutile', "%s : dernière sauvegarde de la carte, pas encore reprise" % neuf, [c], 'appliquer', True,
                                  "comme la carte au démarrage"))
        else:
            lignes.append(LigneSD('inutile', "%s : sauvegarde interrompue (incomplète)" % neuf, [c], 'suppr', True,
                                  "la carte l'ignore déjà"))

    # ---- a garder
    garder = [(('base.bin',), "Base des pubs (empreintes)"), (('infos.bin',), "Fiches des pubs (nom, chaîne, à vérifier)"),
              (('voix',), "Commande vocale"), (('motcle.bin',), "Mot-clé"), (('tv',), "Connexion à la TV (certificats)"),
              (('coupures.csv',), "Historique des coupures"), (('coupures_ancien.csv',), "Historique des coupures (ancien)"),
              (('journal.csv',), "Journal des pubs apprises"), (('archive.ulaw',), "Archive audio des dernières minutes"),
              (('synchro.txt',), "Date de la dernière base reçue du PC")]
    for n, t in garder:
        c = p(*n)
        if os.path.exists(c):
            classer(c)
            lignes.append(LigneSD('garder', t, [c]))

    # ---- sons des pubs
    ad = p('audio')
    if os.path.isdir(ad):
        classer(ad)
        numeros = {x.numero for x in bib.pubs.values()} if bib is not None else None
        bons, orphelins, restes, autres = [], [], [], []
        for f in sorted(os.listdir(ad)):
            c = os.path.join(ad, f)
            classer(c)
            racine_nom, ext = os.path.splitext(f)
            if os.path.isfile(c) and ext.lower() == '.wav' and racine_nom.isdigit():
                if numeros is None or int(racine_nom) in numeros:
                    bons.append(c)
                else:
                    orphelins.append(c)
            elif os.path.isfile(c) and ext.lower() in ('.tmp', '.pc'):
                restes.append(c)
            else:
                autres.append(c)
        lignes.append(LigneSD('garder', "Sons des pubs (%d)" % len(bons), bons))
        if orphelins:
            # base vide mais des sons : on ne coche pas d'office (prudence)
            lignes.append(LigneSD('inutile', "Sons sans pub (%d) : leur pub a été supprimée" % len(orphelins), orphelins, 'suppr',
                                  bool(bib and bib.pubs)))
        if restes:
            lignes.append(LigneSD('inutile', "Sons en cours de découpe, interrompus (%d)" % len(restes), restes, 'suppr', True))
        for c in autres:
            lignes.append(LigneSD('inconnu', "audio/" + os.path.basename(c), [c], 'suppr', False))

    # ---- restes de transferts et d'imports
    restes = [(('_nouvelle',), "Base reçue du PC mais jamais appliquée (_nouvelle)"),
              (('ancienne_base',), "Base précédente gardée au dernier « PC → carte » (ancienne_base)"),
              (('import.pub',), "Fichier d'import déjà importé (import.pub)"),
              (('banque.pub',), "Fichier d'import déjà importé (banque.pub)"),
              (('base.bin.pc',), "Écriture du PC interrompue (base.bin.pc)"),
              (('infos.bin.pc',), "Écriture du PC interrompue (infos.bin.pc)")]
    for n, t in restes:
        c = p(*n)
        if os.path.exists(c):
            classer(c)
            lignes.append(LigneSD('inutile', t, [c], 'suppr', True))
    c = os.path.join(racine, 'sdtest.bin')
    if os.path.isfile(c):
        classer(c)
        lignes.append(LigneSD('inutile', "Fichier du test de vitesse de la carte (sdtest.bin)", [c], 'suppr', True))

    # ---- anciennes sauvegardes Python sur la carte (avant v0.9.54)
    sp = p('sauvegardes_pc')
    if os.path.isdir(sp):
        classer(sp)
        n = sum(1 for x in os.listdir(sp) if os.path.isdir(os.path.join(sp, x)))
        lignes.append(LigneSD('inutile', "Anciennes sauvegardes de gestion_pubs (%d, sauvegardes_pc)" % n, [sp], 'transferer', True,
                              "les %d plus récentes vont dans DetecteurPub\\sauvegardes" % SAUV_GARDEES))

    # ---- sauvegardes de la carte (bouton Sauvegarder du portail) : les 5 dernieres sauv_NNN.pub
    sv = p('sauvegardes')
    if os.path.isdir(sv):
        classer(sv)
        sauv, autres_pub, inconnus = [], [], []
        for f in os.listdir(sv):
            c = os.path.join(sv, f)
            classer(c)
            if os.path.isfile(c) and f.lower().startswith('sauv_') and f.lower().endswith('.pub') and f[5:-4].isdigit():
                sauv.append((int(f[5:-4]), c))
            elif os.path.isfile(c) and f.lower().endswith('.pub'):
                autres_pub.append(c)
            else:
                inconnus.append(c)
        sauv.sort(reverse=True)
        lignes.append(LigneSD('garder', "Sauvegardes de la carte (%d dernières)" % min(len(sauv), SAUV_GARDEES),
                              [c for _, c in sauv[:SAUV_GARDEES]] + autres_pub))
        if len(sauv) > SAUV_GARDEES:
            lignes.append(LigneSD('inutile', "Sauvegardes de la carte plus anciennes (%d)" % (len(sauv) - SAUV_GARDEES),
                                  [c for _, c in sauv[SAUV_GARDEES:]], 'suppr', True))
        for c in inconnus:
            lignes.append(LigneSD('inconnu', "sauvegardes/" + os.path.basename(c), [c], 'suppr', False))

    # ---- captations
    cd = p('captations')
    if os.path.isdir(cd):
        classer(cd)
        cap, fo, poubelles, inconnus = [], [], [], []
        for f in sorted(os.listdir(cd)):
            c = os.path.join(cd, f)
            if f == 'auto' and os.path.isdir(c):
                continue
            classer(c)
            fl = f.lower()
            if os.path.isfile(c) and ((fl.startswith('cap_') and fl.endswith('.wav')) or fl == 'index.csv'):
                cap.append(c)
            elif os.path.isfile(c) and fl.startswith('fo_test') and fl.endswith('.wav'):
                fo.append(c)
            elif fl == '_supprimees':
                poubelles.append(c)
            else:
                inconnus.append(c)
        au = os.path.join(cd, 'auto')
        auto, en_cours = [], []
        if os.path.isdir(au):
            classer(au)
            for f in sorted(os.listdir(au)):
                c = os.path.join(au, f)
                classer(c)
                fl = f.lower()
                if fl == '_en_cours.wav':
                    en_cours.append(c)
                elif fl == '_supprimees':
                    poubelles.append(c)
                elif os.path.isfile(c) and (fl.endswith('.wav') or fl == 'index.csv'):
                    auto.append(c)
                else:
                    inconnus.append(c)
        nwav = lambda l: sum(1 for c in l if c.lower().endswith('.wav'))
        if auto:
            lignes.append(LigneSD('captation', "AUTO : pubs découpées, à traiter (%d)" % nwav(auto), auto, 'suppr', False))
        if [c for c in cap if c.lower().endswith('.wav')]:
            lignes.append(LigneSD('captation', "Captations entières (%d)" % nwav(cap), cap, 'suppr', False))
        elif cap:                                       # index.csv seul : il ne sert plus
            lignes.append(LigneSD('inutile', "Liste de captations vide (index.csv)", cap, 'suppr', True))
        if fo:
            lignes.append(LigneSD('captation', "Test de l'entrée optique (%d)" % len(fo), fo, 'suppr', False))
        if en_cours:
            lignes.append(LigneSD('inutile', "Découpe AUTO interrompue (_en_cours.wav)", en_cours, 'suppr', True))
        if poubelles:
            lignes.append(LigneSD('inutile', "Captations mises de côté par l'ancien gestion_pubs (_supprimees)", poubelles, 'suppr', True))
        for c in inconnus:
            lignes.append(LigneSD('inconnu', os.path.relpath(c, pub).replace('\\', '/'), [c], 'suppr', False))

    # ---- tout le reste du dossier pub, puis de la racine : inconnu (jamais coche d'office)
    for f in sorted(os.listdir(pub)):
        c = p(f)
        if os.path.normcase(os.path.abspath(c)) not in vus:
            vide = os.path.isdir(c) and taille_chemin(c) == 0
            lignes.append(LigneSD('inconnu', "pub/" + f + ("/ (dossier vide)" if vide else ("/" if os.path.isdir(c) else "")), [c], 'suppr', False))
    for f in sorted(os.listdir(racine)):
        c = os.path.join(racine, f)
        if f.lower() == 'pub' or f.lower() in SYSTEME_SD or os.path.normcase(os.path.abspath(c)) in vus:
            continue
        lignes.append(LigneSD('inconnu', f + ("/" if os.path.isdir(c) else ""), [c], 'suppr', False))
    ordre = {'alerte': 0, 'garder': 1, 'inutile': 2, 'captation': 3, 'inconnu': 4}
    lignes.sort(key=lambda l: ordre[l.cat])
    return lignes, resume, bib


def copier_sd(racine, lignes, progres=None):
    """ZIP complet de pub/ (+ les fichiers de la racine qui seront effaces) dans DetecteurPub/sauvegardes_carte_sd.
    Verifie la taille de chaque fichier ecrit. Renvoie le chemin du ZIP."""
    os.makedirs(DOSSIER_COPIES_SD, exist_ok=True)
    fichiers = []
    hors_pub = [c for l in lignes if l.coche and l.action for c in l.chemins
                if not os.path.abspath(c).startswith(os.path.join(os.path.abspath(racine), 'pub'))]
    for base in [os.path.join(racine, 'pub')] + hors_pub:
        if os.path.isfile(base):
            fichiers.append(base)
        for r, _, fs in os.walk(base):
            fichiers += [os.path.join(r, f) for f in fs]
    total = sum(os.path.getsize(c) for c in fichiers) or 1
    libre = shutil.disk_usage(DOSSIER_COPIES_SD).free
    if libre < total + 50 * 1048576:
        raise OSError("pas assez de place sur le PC pour la copie complète (%s nécessaires, %s libres)"
                      % (fmt_taille(total), fmt_taille(libre)))
    sortie = os.path.join(DOSSIER_COPIES_SD, 'sd_complete_%s.zip' % time.strftime('%Y%m%d_%H%M%S'))
    tmp = sortie + '.tmp'
    fait = 0
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED, allowZip64=True) as arc:
        for c in fichiers:
            arc.write(c, os.path.relpath(c, racine).replace('\\', '/'))
            fait += os.path.getsize(c)
            if progres:
                progres(fait / total)
    with zipfile.ZipFile(tmp) as arc:                # controle : chaque fichier present, a la bonne taille
        tailles = {i.filename: i.file_size for i in arc.infolist()}
    for c in fichiers:
        if tailles.get(os.path.relpath(c, racine).replace('\\', '/')) != os.path.getsize(c):
            os.remove(tmp)
            raise OSError("copie incomplète de %s : rien n'a été effacé" % c)
    os.replace(tmp, sortie)
    copies = sorted(n for n in os.listdir(DOSSIER_COPIES_SD) if n.startswith('sd_complete_') and n.endswith('.zip'))
    for n in copies[:-COPIES_SD_GARDEES]:
        os.remove(os.path.join(DOSSIER_COPIES_SD, n))
    return sortie


def appliquer_nettoyage(racine, lignes):
    """traite les lignes cochees (la copie complete est deja faite). Renvoie (octets liberes, erreurs)."""
    pub = os.path.abspath(os.path.join(racine, 'pub'))
    libere, erreurs = 0, []

    def effacer(c):
        if os.path.isdir(c):
            shutil.rmtree(c)
        elif os.path.exists(c):
            os.remove(c)

    for l in lignes:                                   # 1) terminer base.new / infos.new (avant tout le reste)
        if l.coche and l.action == 'appliquer':
            for c in l.chemins:
                try:
                    os.replace(c, c[:-4] + '.bin')
                except OSError as e:
                    erreurs.append("%s : %s" % (os.path.basename(c), e))
    for l in lignes:
        if not l.coche or l.action not in ('suppr', 'transferer'):
            continue
        if l.cat == 'garder':                          # jamais (securite)
            continue
        if l.action == 'transferer':
            for sp in l.chemins:
                try:
                    noms = sorted((n for n in os.listdir(sp) if os.path.isfile(os.path.join(sp, n, 'base.bin'))), reverse=True)
                    os.makedirs(DOSSIER_SAUV, exist_ok=True)
                    for n in noms[:SAUV_GARDEES]:
                        dst = os.path.join(DOSSIER_SAUV, n)
                        if not os.path.exists(dst):
                            shutil.copytree(os.path.join(sp, n), dst)
                except OSError as e:
                    erreurs.append("sauvegardes_pc : %s (laissé sur la carte)" % e)
                    continue
                Bibliotheque.elaguer_sauvegardes()
                t = taille_chemin(sp)
                try:
                    effacer(sp)
                    libere += t
                except OSError as e:
                    erreurs.append("sauvegardes_pc : %s" % e)
            continue
        for c in l.chemins:
            a = os.path.abspath(c)
            # garde-fous : rien hors de la carte, jamais la base ni le dossier pub lui-meme
            if not a.startswith(os.path.abspath(racine) + os.sep) or a == pub or \
                    os.path.basename(a) in ('base.bin', 'infos.bin') and os.path.dirname(a) == pub:
                continue
            t = taille_chemin(c)
            try:
                effacer(c)
                libere += t
            except OSError as e:
                erreurs.append("%s : %s" % (os.path.relpath(c, racine), e))
    return libere, erreurs


def remettre_copie_sd(zip_chemin, racine):
    """remet une copie complete sur la carte : le dossier pub actuel est remplace par celui du ZIP"""
    with zipfile.ZipFile(zip_chemin) as arc:
        noms = [i.filename.replace('\\', '/') for i in arc.infolist()]
        if 'pub/base.bin' not in noms:
            raise ValueError("ce ZIP n'est pas une copie complète de carte SD (pas de pub/base.bin)")
        for n in noms:
            if n.startswith('/') or '..' in n.split('/') or ':' in n:
                raise ValueError("nom refusé : %s" % n)
        pub = os.path.join(racine, 'pub')
        if os.path.isdir(pub):
            shutil.rmtree(pub)
        arc.extractall(racine)


class FenetreComparer:
    """v0.9.56 : deux lecteurs cote a cote : A = le passage de la captation, B = la pub de la base qui lui ressemble"""

    def __init__(self, fen_cap, i, pub, r):
        self.fc, self.app, self.i, self.pub = fen_cap, fen_cap.app, i, pub
        tk, ttk = self.app.tk, self.app.ttk
        f = fen_cap.fichiers[i]
        self.chemin = f['chemin']
        self.cap = fen_cap.cap if fen_cap.cap.chemin == self.chemin else Captation(self.chemin)
        num, votes, cf, cp, sur, debut = r
        d_pub = max(1.0, float(pub.duree_s))
        self.long = self.cap.duree > d_pub * 1.5 + 3
        if self.long:                              # longue captation : le passage qui ressemble
            self.deb = max(0.0, min(debut, self.cap.duree - 1.0))
            self.fin = min(self.cap.duree, self.deb + d_pub)
        else:                                      # morceau AUTO : tout le fichier, sans le silence autour
            self.deb, self.fin = bornes_sonores(self.cap.x)
        self.son_b = fen_cap.app.bib.echantillons(pub)
        self.lecteur = Lecteur()
        self.w = w = tk.Toplevel(fen_cap.w)
        w.title("Comparer à l'oreille")
        w.geometry("1000x520")
        w.transient(fen_cap.w)
        barre_titre_sombre(w, self.app.var_theme.get() == 'sombre')
        verdict = ("✓ Sûr : c'est la même pub (%d %% de la captation, %d %% de la pub)" if sur else
                   "? Pas sûr : seulement %d %% de la captation et %d %% de la pub concordent") % (100 * cf, 100 * cp)
        ttk.Label(w, text=verdict, font=('Segoe UI', 12, 'bold'),
                  foreground='#2e9d4f' if sur else '#d9822b').pack(anchor='w', padx=16, pady=(12, 2))
        ttk.Label(w, text="Écoute A puis B. Même pub ? Le fichier de captation peut être effacé (la base n'est pas touchée).",
                  style='Sous.TLabel').pack(anchor='w', padx=16, pady=(0, 8))
        cols = ttk.Frame(w)
        cols.pack(fill='both', expand=True, padx=16)
        cols.columnconfigure(0, weight=1, uniform='c')
        cols.columnconfigure(1, weight=1, uniform='c')
        self.cv = {}
        self.lbl = {}
        self.b_lire = {}
        self.pos = {'a': 0.0, 'b': 0.0}            # tete de lecture de chaque lecteur (s)
        self.actif = None                          # lecteur en cours : (cle, instant de depart, position de depart)
        self.cache = {}
        for k, (cle, titre) in enumerate((
                ('a', "A · captation : %s\npassage %s → %s" % (os.path.basename(self.chemin), self.app.fmt_fin(self.deb), self.app.fmt_fin(self.fin))),
                ('b', "B · dans la base : pub n°%d « %s »  (%s)" % (pub.numero, pub.nom, CHAINES[pub.chaine] if pub.chaine else "chaîne ?")
                 + "\n%s" % self.app.fmt(pub.duree_s)))):
            fr = ttk.LabelFrame(cols, text="  " + ("A" if cle == 'a' else "B") + "  ", padding=10)
            fr.grid(row=0, column=k, sticky='nsew', padx=(0, 8) if k == 0 else (8, 0))
            ttk.Label(fr, text=titre, justify='left').pack(anchor='w')
            cv = tk.Canvas(fr, height=110, bg=self.app.couleurs()['fond'], highlightthickness=0)
            cv.pack(fill='x', pady=8)
            cv.bind('<Configure>', lambda e, c=cle: self.dessiner(c))
            cv.bind('<Button-1>', lambda e, c=cle: self.clic_onde(c, e))
            self.cv[cle] = cv
            lj = ttk.Frame(fr)
            lj.pack(fill='x')
            ttk.Button(lj, text="⏮", width=3, command=lambda c=cle: self.aller(c, 0.0)).pack(side='left', ipady=4)
            ttk.Button(lj, text="⏪ 3 s", command=lambda c=cle: self.decaler(c, -3)).pack(side='left', padx=(4, 0), ipady=4)
            self.b_lire[cle] = ttk.Button(lj, text="▶  " + cle.upper(), width=7, command=lambda c=cle: self.basculer(c),
                                          style='Accent.TButton' if self.app.sv else 'TButton')
            self.b_lire[cle].pack(side='left', padx=4, ipady=4)
            ttk.Button(lj, text="3 s ⏩", command=lambda c=cle: self.decaler(c, 3)).pack(side='left', ipady=4)
            autre = 'B' if cle == 'a' else 'A'
            ttk.Button(lj, text="⇄ Même endroit " + autre, command=lambda c=cle: self.meme_endroit(c)).pack(side='left', padx=(8, 0), ipady=4)
            self.lbl[cle] = ttk.Label(fr, text="", font=('Consolas', 11))
            self.lbl[cle].pack(anchor='e', pady=(4, 0))
            if cle == 'a':
                ttk.Button(fr, text="✓  Garder A (la captation)", command=self.garder_a).pack(fill='x', pady=(10, 0), ipady=6)
                ttk.Label(fr, text="remplace la pub n°%d de la base (même numéro, nom, chaîne, détections), "
                          "puis efface le fichier" % pub.numero + (" (sauf captation longue)" if self.long else ""),
                          style='Sous.TLabel', wraplength=440, justify='left').pack(anchor='w', pady=(4, 0))
            else:
                ttk.Button(fr, text="✓  Garder B (la base)", command=self.garder_b).pack(fill='x', pady=(10, 0), ipady=6)
                ttk.Label(fr, text=("la base ne change pas ; la captation longue est gardée (autres pubs dedans)" if self.long
                                    else "la base ne change pas ; le fichier de captation est effacé"),
                          style='Sous.TLabel', wraplength=440, justify='left').pack(anchor='w', pady=(4, 0))
        if self.long:
            ttk.Label(w, text="⚠  Captation bien plus longue que la pub : elle contient sans doute d'autres pubs. "
                      "Ne l'efface pas pour ça : « Voir ce passage », puis ajoute les autres pubs.",
                      foreground='#c42b1c', wraplength=960).pack(anchor='w', padx=16, pady=(10, 0))
        bas = ttk.Frame(w)
        bas.pack(fill='x', padx=16, pady=12)
        ttk.Button(bas, text="▶  A puis B (du début)", command=self.a_puis_b).pack(side='left', ipady=4)
        ttk.Button(bas, text="🔍  Voir ce passage dans la captation", command=self.voir).pack(side='left', padx=8, ipady=4)
        ttk.Button(bas, text="Fermer sans rien faire", command=self.fermer).pack(side='right', ipady=4)
        w.protocol('WM_DELETE_WINDOW', self.fermer)
        self.gen = 0
        self.dernier = 'a'                         # le lecteur que les touches commandent
        w.bind('<space>', lambda e: (self.basculer(self.dernier), 'break')[1])
        w.bind('<Left>', lambda e: (self.decaler(self.dernier, -3), 'break')[1])
        w.bind('<Right>', lambda e: (self.decaler(self.dernier, 3), 'break')[1])
        w.bind('<Tab>', lambda e: (self.meme_endroit(self.dernier), 'break')[1])
        w.bind('<Escape>', lambda e: self.stop())
        w.focus_set()
        for c in ('a', 'b'):
            self.maj_temps(c)

    def sons(self, cle):
        """(echantillons int16 numpy, frequence) de A ou B, volume remonte comme ailleurs (calcule une fois)"""
        if cle in self.cache:
            return self.cache[cle]
        self.cache[cle] = self._sons(cle)
        return self.cache[cle]

    def duree(self, cle):
        x, fs = self.sons(cle)
        return len(x) / float(fs)

    def _sons(self, cle):
        if cle == 'a':
            x = self.cap.x[int(self.deb * Captation.FS):int(self.fin * Captation.FS)]
            fs = Captation.FS
        else:
            x = np.array(self.son_b if self.son_b is not None else [], dtype=np.int16)
            fs = FS_WAV
        g = gain_auto_np(x) if self.app.var_vol.get() else 1.0
        if g != 1.0 and len(x):
            x = np.clip(x.astype(np.float32) * g, -32768, 32767).astype(np.int16)
        return x, fs

    def dessiner(self, cle):
        cv = self.cv[cle]
        cv.delete('all')
        x, _ = self.sons(cle)
        lg, ht = max(10, cv.winfo_width()), max(10, cv.winfo_height())
        if len(x) == 0:
            cv.create_text(lg // 2, ht // 2, text="pas de son", fill=self.app.couleurs()['texte'])
            return
        pas = max(1, len(x) // lg)
        n = len(x) // pas
        r = x[:n * pas].reshape(n, pas)
        mn, mx = r.min(axis=1) / 32768.0, r.max(axis=1) / 32768.0
        c = self.app.couleurs()['onde']
        for k in range(n):
            px = k * lg / n
            cv.create_line(px, ht / 2 - mx[k] * ht / 2, px, ht / 2 - mn[k] * ht / 2 + 1, fill=c)
        self.curseur(cle)

    def curseur(self, cle):
        cv = self.cv[cle]
        cv.delete('tete')
        d = self.duree(cle)
        if d <= 0:
            return
        px = self.position(cle) / d * max(10, cv.winfo_width())
        cv.create_line(px, 0, px, max(10, cv.winfo_height()), fill=self.app.couleurs()['debut'], width=2, tags='tete')

    def position(self, cle):
        if self.actif and self.actif[0] == cle:
            return min(self.duree(cle), self.actif[2] + time.time() - self.actif[1])
        return self.pos[cle]

    def maj_temps(self, cle):
        try:
            self.lbl[cle].configure(text="%s / %s" % (self.app.fmt_fin(self.position(cle)), self.app.fmt_fin(self.duree(cle))))
        except Exception:
            pass

    def clic_onde(self, cle, ev):
        d = self.duree(cle)
        self.aller(cle, d * ev.x / max(10, self.cv[cle].winfo_width()), lire=True)

    def aller(self, cle, t, lire=None):
        """place la tete de lecture ; relance la lecture si ce lecteur jouait (ou si lire=True)"""
        jouait = bool(self.actif and self.actif[0] == cle)
        self.pos[cle] = max(0.0, min(t, max(0.0, self.duree(cle) - 0.2)))
        self.dernier = cle
        if lire or (lire is None and jouait):
            self.jouer(cle)
        else:
            if jouait:
                self.stop()
            self.curseur(cle)
            self.maj_temps(cle)

    def decaler(self, cle, ds):
        self.aller(cle, self.position(cle) + ds)

    def basculer(self, cle):
        self.dernier = cle
        if self.actif and self.actif[0] == cle:   # pause : on garde l'endroit
            self.pos[cle] = self.position(cle)
            self.stop()
        else:
            if self.pos[cle] >= self.duree(cle) - 0.3:
                self.pos[cle] = 0.0
            self.jouer(cle)

    def meme_endroit(self, cle):
        """ecoute l'autre lecteur au meme moment de la pub (les deux commencent au debut de la pub)"""
        autre = 'b' if cle == 'a' else 'a'
        t = self.position(cle)
        self.pos[autre] = max(0.0, min(t, self.duree(autre) - 0.3))
        self.dernier = autre
        self.jouer(autre)

    def jouer(self, cle, ensuite=None):
        if self.actif and self.actif[0] == cle:   # meme lecteur : c'est la tete demandee (self.pos) qui compte
            self.actif = None
        self.stop()
        x, fs = self.sons(cle)
        if len(x) == 0:
            return
        depart = self.pos[cle]
        bout = x[int(depart * fs):]
        if len(bout) < fs // 10:
            return
        self.lecteur.jouer(bout, fs)
        self.gen += 1
        g = self.gen
        self.actif = (cle, time.time(), depart)
        self.dernier = cle
        self.b_lire[cle].configure(text="❚❚  " + cle.upper())
        fin_t = time.time() + len(bout) / float(fs)

        def suivre():
            if g != self.gen:
                return
            if time.time() >= fin_t:
                self.actif = None
                self.pos[cle] = 0.0
                try:
                    self.b_lire[cle].configure(text="▶  " + cle.upper())
                    self.curseur(cle)
                    self.maj_temps(cle)
                except Exception:
                    return
                if ensuite:
                    ensuite()
                return
            try:
                self.curseur(cle)
                self.maj_temps(cle)
            except Exception:
                return
            self.w.after(60, suivre)
        suivre()

    def a_puis_b(self):
        self.pos['a'] = self.pos['b'] = 0.0
        self.jouer('a', ensuite=lambda: self.jouer('b'))

    def stop(self):
        if self.actif:
            c = self.actif[0]
            self.pos[c] = self.position(c)
        self.actif = None
        self.gen += 1
        self.lecteur.arreter()
        for c, b in self.b_lire.items():
            try:
                b.configure(text="▶  " + c.upper())
                self.curseur(c)
                self.maj_temps(c)
            except Exception:                      # fenetre deja fermee
                pass

    def voir(self):
        """ouvre ce fichier dans la fenetre de captation, passage choisi"""
        fc = self.fc
        self.fermer()
        if fc.cur != self.i:
            fc.charger(self.i)
        fc.sel = [round(self.deb, 1), round(self.fin, 1)]
        fc.var_deb.set(fc.sel[0])
        fc.var_fin.set(fc.sel[1])
        fc.vue = [max(0.0, self.deb - 5), min(fc.cap.duree, self.fin + 5)]
        fc.maj_duree()
        fc.dessiner()

    def effacer_fichier(self):
        try:
            os.remove(self.chemin)
            return True
        except OSError as e:
            self.app.mb.showerror("Effacer", str(e), parent=self.w)
            return False

    def garder_b(self):
        """la pub de la base est la bonne : la captation (courte) est effacee"""
        fc, app = self.fc, self.app
        self.stop()
        if self.long:
            self.fermer()
            app.dire("Base inchangée ; « %s » est gardée (elle contient d'autres pubs)." % os.path.basename(self.chemin))
            return
        if not self.effacer_fichier():
            return
        fc.fichiers[self.i]['etat'] = 'supprimée'
        self.fermer()
        app.dire("Gardé B : la pub n°%d de la base ne change pas ; « %s » effacé." % (self.pub.numero, os.path.basename(self.chemin)))
        fc.apres_lot()

    def garder_a(self):
        """la captation est meilleure : elle remplace la pub de la base (meme numero), puis le fichier est efface"""
        fc, app, bib, p = self.fc, self.app, self.app.bib, self.pub
        if not (FenetreCaptation.MIN_S <= self.fin - self.deb <= FenetreCaptation.MAX_S):
            app.mb.showwarning("Garder A", "Le passage doit durer entre 5 s et 7 min (là : %.1f s)." % (self.fin - self.deb), parent=self.w)
            return
        if not app.mb.askyesno("Garder A",
                               "Remplacer la pub n°%d « %s » de la base par la captation (%s → %s) ?\n\n"
                               "Gardés : numéro, nom, chaîne, nombre de détections.\nRemplacés : le son et les empreintes.\n%s"
                               "Ctrl+Z dans la fenêtre principale annule le remplacement."
                               % (p.numero, p.nom, app.fmt_fin(self.deb), app.fmt_fin(self.fin),
                                  "La captation longue est gardée (autres pubs dedans).\n" if self.long
                                  else "Le fichier de captation est ensuite effacé.\n"), parent=self.w):
            return
        self.stop()
        try:
            self.w.configure(cursor='watch')
            self.w.update_idletasks()
            h, t, t0, t1, son8 = self.cap.passage(self.deb, self.fin)
            app.photo()
            bib.remplacer_pub(p, h, t, t0, t1, son8)
        except ValueError as e:
            app.mb.showerror("Garder A", str(e), parent=self.w)
            return
        finally:
            try:
                self.w.configure(cursor='')
            except Exception:
                pass
        f = fc.fichiers[self.i]
        if not self.long and self.effacer_fichier():
            f['etat'] = 'supprimée'
        elif not self.long:
            f['etat'] = "✓ remplace n°%d" % p.numero
        app.remplir()
        self.fermer()
        app.dire("Gardé A : la pub n°%d est remplacée par la captation. « ✅ » pour enregistrer." % p.numero)
        fc.apres_lot()
        fc.analyser()

    def fermer(self):
        self.stop()
        try:
            self.w.destroy()
        except Exception:
            pass


class FenetreNettoyage:
    """v0.9.55 : liste en couleurs de ce qui est sur la carte SD, puis nettoyage (copie complete d'abord)"""
    COULEURS = {'garder': '#2e9d4f', 'inutile': '#d9822b', 'captation': '#3a7bd5', 'inconnu': '#8a8a8a', 'alerte': '#e81123'}
    ICONES = {'garder': '🟢', 'inutile': '🟠', 'captation': '🔵', 'inconnu': '⚪', 'alerte': '🔴'}

    def __init__(self, app, racine):
        import threading
        self.threading = threading
        self.app, self.racine = app, racine
        self.tk, self.ttk = app.tk, app.ttk
        tk, ttk = self.tk, self.ttk
        self.w = w = tk.Toplevel(app.r)
        w.title("Nettoyer la carte SD · " + racine)
        w.geometry("1120x680")
        w.transient(app.r)
        barre_titre_sombre(w, app.var_theme.get() == 'sombre')
        self.lbl_resume = ttk.Label(w, text="", font=('Segoe UI', 11, 'bold'))
        self.lbl_resume.pack(anchor='w', padx=14, pady=(12, 2))
        ttk.Label(w, style='Sous.TLabel', justify='left', text=(
            "🟢 gardé, on n'y touche jamais   🟠 inutile, coché   🔵 captations pas encore traitées, non cochées   "
            "⚪ inconnu, non coché\nClique sur une ligne pour la cocher ou la décocher. "
            "Avant d'effacer, toute la carte est copiée sur le PC :\nDetecteurPub\\sauvegardes_carte_sd "
            "(les %d dernières copies sont gardées)." % COPIES_SD_GARDEES)).pack(anchor='w', padx=14)
        cadre = ttk.Frame(w)
        cadre.pack(fill='both', expand=True, padx=14, pady=8)
        self.tv = ttk.Treeview(cadre, columns=('c', 'quoi', 'taille', 'fait'), show='headings', selectmode='browse')
        for col, t, l, a in (('c', "", 40, 'center'), ('quoi', "Sur la carte", 580, 'w'), ('taille', "Taille", 90, 'e'),
                             ('fait', "Ce qui sera fait", 320, 'w')):
            self.tv.heading(col, text=t)
            self.tv.column(col, width=l, anchor=a, stretch=(col == 'quoi'))
        sb = ttk.Scrollbar(cadre, orient='vertical', command=self.tv.yview)
        self.tv.configure(yscrollcommand=sb.set)
        self.tv.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')
        for cat, c in self.COULEURS.items():
            self.tv.tag_configure(cat, foreground=c)
        self.tv.bind('<ButtonRelease-1>', self.clic)
        self.tv.bind('<space>', self.clic)
        bas = ttk.Frame(w)
        bas.pack(fill='x', padx=14, pady=(0, 12))
        self.lbl_total = ttk.Label(bas, text="")
        self.lbl_total.pack(side='left')
        self.barre = ttk.Progressbar(bas, length=200, mode='determinate', maximum=1.0)
        ttk.Button(bas, text="Fermer", command=w.destroy).pack(side='right')
        self.b_net = ttk.Button(bas, text="🧹  Nettoyer", command=self.nettoyer, style='Accent.TButton' if app.sv else 'TButton')
        self.b_net.pack(side='right', padx=8)
        self.b_cap = ttk.Button(bas, text="🎙  Traiter les captations d'abord", command=self.traiter_captations)
        self.b_cap.pack(side='right')
        self.analyser()

    def analyser(self):
        self.lignes, resume, self.bib = analyser_sd(self.racine)
        self.lbl_resume.configure(text=resume, foreground=self.COULEURS['alerte'] if self.bib is None else '')
        self.tv.delete(*self.tv.get_children())
        for i, l in enumerate(self.lignes):
            self.tv.insert('', 'end', iid=str(i), tags=(l.cat,), values=self.valeurs(l))
        if not any(l.cat == 'captation' for l in self.lignes):
            self.b_cap.pack_forget()
        self.total()

    def valeurs(self, l):
        c = ("☑" if l.coche else "☐") if l.modifiable else ""
        quoi = self.ICONES[l.cat] + "  " + l.titre
        fait = (l.fait() if l.coche else "laisser") if l.modifiable else ("garder" if l.cat == 'garder' else "")
        if l.detail and l.modifiable and l.coche:
            fait += " · " + l.detail
        return (c, quoi, fmt_taille(l.taille) if l.chemins else "", fait)

    def clic(self, ev=None):
        iid = self.tv.focus() if ev is None or ev.type == self.tk.EventType.KeyPress else self.tv.identify_row(ev.y)
        if not iid:
            return
        l = self.lignes[int(iid)]
        if not l.modifiable:
            return
        l.coche = not l.coche
        self.tv.item(iid, values=self.valeurs(l))
        self.total()

    def total(self):
        sel = [l for l in self.lignes if l.coche and l.modifiable]
        t = sum(l.taille for l in sel if l.action != 'appliquer')
        self.lbl_total.configure(text=("%d élément(s) coché(s) · %s à libérer" % (len(sel), fmt_taille(t))) if sel else "Rien de coché.")
        self.b_net.configure(state='normal' if sel else 'disabled')

    def traiter_captations(self):
        """ouvre la carte (ancien mode) puis les captations ; relancer le nettoyage ensuite"""
        self.w.destroy()
        app = self.app
        if not (app.bib and not app.mode_zip and os.path.abspath(app.bib.dossier) == os.path.abspath(os.path.join(self.racine, 'pub'))):
            app.ouvrir(self.racine)
        if app.bib:
            app.dire("Traite les captations, enregistre (Ctrl + S), puis relance « Nettoyer la carte SD ».")
            app.ouvrir_captation()

    def nettoyer(self):
        app, mb = self.app, self.app.mb
        sel = [l for l in self.lignes if l.coche and l.modifiable]
        if not sel:
            return
        t = sum(l.taille for l in sel if l.action != 'appliquer')
        cap = [l for l in sel if l.cat in ('captation', 'inconnu')]
        texte = "%d élément(s), %s libérés.\n\n" % (len(sel), fmt_taille(t))
        if cap:
            texte += "Dont, cochés par toi :\n" + "\n".join("  • " + l.titre for l in cap[:8]) + "\n\n"
        texte += ("D'abord, toute la carte est copiée sur le PC (DetecteurPub\\sauvegardes_carte_sd).\n"
                  "Le détecteur doit être éteint et la carte ne doit pas être retirée pendant l'opération.\n\nNettoyer ?")
        if not mb.askyesno("Nettoyer la carte SD", texte, parent=self.w):
            return
        for b in (self.b_net, self.b_cap):
            b.configure(state='disabled')
        self.barre.pack(side='left', padx=12)
        self.etat = {'p': 0.0, 'fin': False, 'erreur': None, 'zip': None, 'libere': 0, 'erreurs': []}
        self.lbl_total.configure(text="Copie complète de la carte…")

        def travail():
            try:
                self.etat['zip'] = copier_sd(self.racine, sel, lambda p: self.etat.__setitem__('p', p))
                self.etat['libere'], self.etat['erreurs'] = appliquer_nettoyage(self.racine, sel)
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                self.etat['erreur'] = str(e)
            self.etat['fin'] = True
        self.threading.Thread(target=travail, daemon=True).start()
        self.suivre()

    def suivre(self):
        e = self.etat
        if not e['fin']:
            self.barre['value'] = e['p']
            self.lbl_total.configure(text="Copie complète de la carte… %d %%" % int(e['p'] * 100) if e['p'] < 1 else "Nettoyage…")
            self.w.after(150, self.suivre)
            return
        self.barre.pack_forget()
        app, mb = self.app, self.app.mb
        if e['erreur']:
            mb.showerror("Nettoyage", "%s\n\nRien n'a été effacé sur la carte." % e['erreur'] if not e['zip'] else e['erreur'], parent=self.w)
            self.analyser()
            self.b_cap.configure(state='normal')
            return
        # la carte etait ouverte (ancien mode) : on la relit
        if app.bib and not app.mode_zip and os.path.abspath(app.bib.dossier) == os.path.abspath(os.path.join(self.racine, 'pub')):
            app.bib.modifie = False
            app.ouvrir(self.racine)
        self.analyser()
        msg = "Carte nettoyée : %s libérés.\n\nCopie complète d'avant : %s" % (fmt_taille(e['libere']), e['zip'])
        if e['erreurs']:
            msg += "\n\nNon traités :\n" + "\n".join(e['erreurs'][:10])
        msg += ("\n\nSi quelque chose manque : Fichier → Carte SD branchée sur le PC → "
                "« Remettre une copie complète sur la carte SD ».")
        app.dire("Carte SD nettoyée (%s libérés)." % fmt_taille(e['libere']))
        mb.showinfo("Nettoyage terminé", msg, parent=self.w)


# ---------------------------------------------------------------- interface (Windows 11)
def preparer_windows():
    """texte net sur les ecrans haute resolution de Windows 10 / 11"""
    if sys.platform.startswith('win'):
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass


def barre_titre_sombre(fenetre, sombre):
    """barre de titre Windows 11 assortie au theme (sombre ou claire)"""
    if not sys.platform.startswith('win'):
        return
    try:
        import ctypes
        fenetre.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(fenetre.winfo_id())
        v = ctypes.c_int(1 if sombre else 0)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(v), ctypes.sizeof(v))
    except Exception:
        pass


AIDE_CLAVIER = """Espace\t\tLire / pause (reprend où on s'est arrêté)
← / →\t\tReculer / avancer de 2 s
D / F\t\tDébut / fin du passage à garder, à l'endroit écouté
Clic\t\tAller à cet endroit de la forme du son
Entrée\t\tOK, c'est une pub (et pub suivante)
Suppr\t\tSupprimer la pub
N\t\tPasser à la suivante sans rien changer
F2\t\tRenommer
Ctrl + Z\t\tAnnuler la dernière action
Ctrl + S\t\tEnregistrer et préparer l'envoi à la carte
Ctrl + O\t\tOuvrir la base (ZIP du portail)
Ctrl + I\t\tTraiter des captations (ZIP du portail ou WAV)
Ctrl + F\t\tChercher
Ctrl + R\t\tCommencer la revue des pubs « à vérifier »
F5\t\tRelire la carte SD
Échap\t\tArrêter le son

Sur la forme du son : glisser le trait jaune (début) ou rouge (fin) ; clic droit = menu.

Découper une pub : Lire, appuyer sur D au vrai début de la pub, F à sa fin,
vérifier avec « Autour du début » et « Avant la fin », puis « ✂ Découper »."""

AIDE_MODES = """DEUX FAÇONS DE TRAVAILLER (choisies sur l'écran d'accueil)

💾 MODE 1 : LA CARTE SD EST DANS LE PC
1. Débranche le détecteur, retire la carte SD, mets-la dans le lecteur du PC.
2. « 💾 Ouvrir la carte SD » : elle est trouvée toute seule.
   Ancien format ? Je propose de la remettre à niveau (copie complète sur le PC avant).
3. « ⚠ Vérifier les pubs » : Espace écouter · Entrée OK · Suppr supprimer · N passer.
   « 🎙 Captations » : les captations de la carte s'ouvrent.
4. « ✅ Enregistrer sur la carte », puis « ⏏ Éjecter ».
5. Remets la carte dans le détecteur éteint, puis rebranche-le.

🌐 MODE 2 : PAR LE PORTAIL (fichier .zip), la carte SD reste dans le détecteur
1. Portail : Réglages → Base complète → « ⬇ Carte → PC ».
   (Pour des captations : Capter → Fichiers de captation → « ⬇ Tout télécharger ».)
2. « 📚 Vérifier les pubs » ou « 🎙 Ajouter des pubs depuis des captations » :
   le fichier téléchargé est trouvé tout seul.
3. Vérifie ou ajoute les pubs comme au mode 1.
4. « ✅ Terminer : préparer l'envoi » : le dossier du fichier à renvoyer s'ouvre.
5. Portail : « ⬆ PC → carte », choisis base_corrigee.zip, puis « Remplacer la base ».
   Captations : « Vider » le groupe sur le portail.

Les chiffres (pubs · à vérifier · détections) sont les mêmes ici, sur le portail et sur l'écran du détecteur.
Tout ce qui est rare est dans « ⋯ Plus »."""

AIDE_CARTE = """1. Éteins le détecteur (débranche-le), puis retire la carte SD.
2. Mets-la dans le PC, puis « Fichier → Ouvrir la carte SD… » et choisis sa lettre (ex. E:).
3. Fais ta revue : écoute, OK, renommer, chaîne, découper, supprimer.
4. « Fichier → Enregistrer sur la carte SD » (Ctrl + S).
   Une copie de l'ancienne bibliothèque est gardée dans DetecteurPub/sauvegardes (les 5 dernières).
5. « Fichier → Éjecter la carte SD », attends le message de Windows.
6. Remets la carte SD dans le détecteur, puis rebranche-le.
   Au démarrage, il relit la bibliothèque corrigée (onglet Pubs de l'écran).

Ne remets jamais la carte dans le détecteur allumé, et ne la retire jamais
pendant que le détecteur fonctionne : il peut être en train d'écrire."""


class Application:
    def __init__(self, dossier=None):
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox, simpledialog
        self.tk, self.ttk, self.fd, self.mb, self.sd = tk, ttk, filedialog, messagebox, simpledialog
        preparer_windows()
        self.r = tk.Tk()
        self.r.title("Détecteur de pub — Bibliothèque · " + VERSION)
        self.r.geometry("1380x780")
        self.r.minsize(1100, 640)
        self.bib = None
        self.pub = None
        self.son = None
        self.gain = 1.0
        self.sel = [0.0, 0.0]
        self.jeu_t0 = None
        self.jeu_gen = 0
        self.jeu_debut = self.jeu_duree = 0.0
        self.tete = 0.0                                  # tete de lecture (s) : Espace reprend d'ici
        self.annuler_pile = []
        self.revue = False
        self.mode_zip = False                            # v0.9.54 : base ouverte depuis le ZIP du portail
        self.zip_date = None                             # date du ZIP (base telechargee le ...)
        self.base_zip = None                             # v0.9.56 : le .zip de la base ouverte
        self.lecteur = Lecteur()
        self.tri = {'col': 'num', 'inverse': True}

        # theme Windows 11 (sv-ttk) si present
        self.sv = None
        try:
            import sv_ttk
            self.sv = sv_ttk
        except ImportError:
            pass
        self.var_theme = tk.StringVar(value='clair')
        self.var_sombre = tk.BooleanVar(value=False)
        self.page = 'accueil'
        self.var_auto = tk.BooleanVar(value=False)       # lire a la selection
        self.var_vol = tk.BooleanVar(value=True)         # volume automatique
        self.var_filtre = tk.StringVar(value='toutes')
        self.var_cherche = tk.StringVar()
        self.appliquer_theme()

        self.construire_menus()
        self.construire_barre()
        self.construire_corps()
        self.construire_statut()
        self.construire_accueil()
        self.raccourcis()
        self.r.protocol('WM_DELETE_WINDOW', self.quitter)
        self.montrer('accueil')
        if dossier:
            self.r.after(150, lambda: self.ouvrir(dossier))

    # ================================================================ apparence
    def couleurs(self):
        sombre = self.var_theme.get() == 'sombre'
        return {'fond': '#1c1c1c' if sombre else '#fbfbfb', 'onde': '#4cc2ff' if sombre else '#0067c0',
                'hors': '#3a3a3a' if sombre else '#c8c8c8', 'texte': '#9a9a9a' if sombre else '#6b6b6b',
                'debut': '#ffb900', 'fin': '#e81123', 'curseur': '#ffffff' if sombre else '#000000',
                'verif': '#3d3420' if sombre else '#fff4ce'}

    def appliquer_theme(self):
        ttk = self.ttk
        sombre = self.var_theme.get() == 'sombre'
        st = ttk.Style()
        if self.sv:
            self.sv.set_theme('dark' if sombre else 'light')
        elif 'vista' in st.theme_names():
            st.theme_use('vista')
        police = ('Segoe UI Variable Text', 10) if sys.platform.startswith('win') else ('TkDefaultFont', 10)
        st.configure('.', font=police)
        st.configure('Treeview', rowheight=30)
        st.configure('Titre.TLabel', font=(police[0], 16, 'bold'))
        st.configure('Sous.TLabel', foreground='#808080')
        st.configure('Statut.TLabel', padding=(10, 4))
        barre_titre_sombre(self.r, sombre)
        if hasattr(self, 'liste'):
            self.liste.tag_configure('verif', background=self.couleurs()['verif'])
            self.onde.configure(bg=self.couleurs()['fond'])
            self.dessiner()

    # ================================================================ menus
    def construire_menus(self):
        """v0.9.56 : plus de barre de menus. Tout ce qui sert souvent est en gros boutons ;
        le reste (rare) est dans un seul menu « ⋯ Plus », sans sous-menus."""
        tk = self.tk
        self.r.configure(menu='')
        p = self.menu_plus = tk.Menu(self.r, tearoff=0)
        p.add_command(label="Choisir moi-même le fichier .zip de la base…", command=lambda: self.ouvrir_zip(choisir=True))
        p.add_command(label="Choisir moi-même les captations (.zip ou .wav)…", command=lambda: self.ouvrir_captation(choisir=True))
        p.add_command(label="Choisir moi-même la carte SD…", command=lambda: self.ouvrir_sd(choisir=True))
        p.add_separator()
        p.add_command(label="Revenir à une sauvegarde de la base…", command=self.restaurer)
        p.add_command(label="Remettre une copie complète sur la carte SD…", command=self.remettre_sd)
        p.add_command(label="Chercher les pubs en double…", command=self.doublons)
        p.add_command(label="Ouvrir le dossier DetecteurPub sur le PC", command=lambda: (os.makedirs(DOSSIER_PC, exist_ok=True), ouvrir_explorateur(DOSSIER_PC)))
        p.add_separator()
        p.add_checkbutton(label="Écouter la pub dès qu'on la choisit", variable=self.var_auto)
        p.add_checkbutton(label="Volume automatique (remonte les pubs enregistrées bas)", variable=self.var_vol, command=self.recharger_son)
        p.add_checkbutton(label="Thème sombre", variable=self.var_sombre,
                          command=lambda: (self.var_theme.set('sombre' if self.var_sombre.get() else 'clair'), self.appliquer_theme()))
        p.add_separator()
        p.add_command(label="Mode d'emploi", command=lambda: self.mb.showinfo("Mode d'emploi", AIDE_MODES))
        p.add_command(label="Raccourcis clavier", command=lambda: self.mb.showinfo("Raccourcis clavier", AIDE_CLAVIER))

        # clic droit sur une pub de la liste
        self.menu_ctx = tk.Menu(self.r, tearoff=0)
        self.menu_ctx.add_command(label="Écouter", command=lambda: self.ecouter(False))
        self.menu_ctx.add_command(label="OK, c'est une pub", command=self.ok)
        self.menu_ctx.add_command(label="Remettre « à vérifier »", command=self.a_verifier)
        self.menu_ctx.add_command(label="Renommer…", command=self.renommer)
        self.menu_ctx.add_separator()
        self.menu_ctx.add_command(label="Supprimer", command=self.supprimer)

    # ================================================================ barre d'outils
    def construire_barre(self):
        """v0.9.56 : une seule rangee de gros boutons ; leur texte suit le mode (carte SD ou portail)"""
        ttk = self.ttk
        b = ttk.Frame(self.r, padding=(12, 10, 12, 4))
        b.pack(fill='x')
        self.barre = b
        self.btn = {}

        def bouton(cle, texte, cmd, accent=False, gauche=True):
            w = ttk.Button(b, text=texte, command=cmd, style='Accent.TButton' if (accent and self.sv) else 'TButton')
            w.pack(side='left' if gauche else 'right', padx=(0, 8) if gauche else (8, 0), ipady=4)
            self.btn[cle] = w
            return w

        bouton('accueil', "🏠  Accueil", lambda: self.montrer('accueil'))
        bouton('revue', "⚠  Vérifier les pubs", self.commencer_revue)
        bouton('captation', "🎙  Captations", self.ouvrir_captation)
        bouton('sauver', "✅  Terminer", self.enregistrer, accent=True)
        bouton('ejecter', "⏏  Éjecter", self.ejecter)
        bouton('annuler', "↶", self.annuler).configure(width=3)
        # a droite : Plus, recherche, filtre
        mb = ttk.Menubutton(b, text="⋯  Plus", menu=self.menu_plus, direction='below')
        mb.pack(side='right', ipady=4)
        ent = ttk.Entry(b, textvariable=self.var_cherche, width=14)
        ent.pack(side='right', padx=(0, 10))
        self.ent_cherche = ent
        ttk.Label(b, text="🔍").pack(side='right', padx=(10, 4))
        for v, t in (('ok', "OK"), ('verif', "À vérifier"), ('toutes', "Toutes")):
            rb = ttk.Radiobutton(b, text=t, value=v, variable=self.var_filtre, command=self.remplir,
                                 style='Toggle.TButton' if self.sv else 'TRadiobutton')
            rb.pack(side='right', padx=2)
        self.var_cherche.trace_add('write', lambda *a: self.remplir())
        # bandeau : le mode en cours et les chiffres, toujours visibles
        self.bandeau = ttk.Frame(self.r, padding=(14, 6, 14, 6))
        self.lbl_mode = ttk.Label(self.bandeau, text="", font=('Segoe UI', 11, 'bold'))
        self.lbl_mode.pack(side='left')
        self.lbl_chiffres = ttk.Label(self.bandeau, text="", font=('Segoe UI', 11))
        self.lbl_chiffres.pack(side='right')

    # ================================================================ liste + fiche
    def construire_accueil(self):
        """v0.9.56 : l'ecran d'accueil (dans la fenetre) : les deux modes, chacun ses gros boutons"""
        tk, ttk = self.tk, self.ttk
        a = self.page_acc = ttk.Frame(self.r, padding=(40, 18, 40, 10))
        ttk.Label(a, text="Détecteur de pub  ·  gestion des pubs sur le PC", font=('Segoe UI', 17, 'bold')).pack(anchor='w')
        self.lbl_acc_chiffres = ttk.Label(a, text="", style='Sous.TLabel', font=('Segoe UI', 11))
        self.lbl_acc_chiffres.pack(anchor='w', pady=(2, 14))
        cols = ttk.Frame(a)
        cols.pack(fill='both', expand=True)
        cols.columnconfigure(0, weight=1, uniform='c')
        cols.columnconfigure(1, weight=1, uniform='c')

        def colonne(i, titre, sous, boutons):
            f = ttk.LabelFrame(cols, text="  " + titre + "  ", padding=18)
            f.grid(row=0, column=i, sticky='nsew', padx=(0, 14) if i == 0 else (14, 0))
            ttk.Label(f, text=sous, style='Sous.TLabel', justify='left', wraplength=470).pack(anchor='w', pady=(0, 14))
            for k, (texte, aide, cmd) in enumerate(boutons):
                ttk.Button(f, text=texte, command=cmd, style='Accent.TButton' if (self.sv and k == 0) else 'TButton').pack(fill='x', ipady=10)
                ttk.Label(f, text=aide, style='Sous.TLabel', justify='left', wraplength=470).pack(anchor='w', pady=(4, 16))
            return f

        colonne(0, "💾  MODE 1 : la carte SD est dans le PC",
                "Tu as éteint le détecteur, retiré la carte SD et tu l'as mise dans le lecteur du PC.\n"
                "Les changements sont écrits directement sur la carte.",
                [("💾  Ouvrir la carte SD", "La carte est trouvée toute seule. Si elle est à l'ancien format, "
                  "je te propose de la remettre à niveau (copie complète sur le PC avant).", lambda: self.ouvrir_sd()),
                 ("🧹  Nettoyer la carte SD", "Voir ce qui est sur la carte et effacer ce qui ne sert plus.", self.nettoyer_sd)])
        colonne(1, "🌐  MODE 2 : par le portail (fichier .zip)",
                "La carte SD reste dans le détecteur. Tu télécharges un fichier .zip depuis la page web du détecteur, "
                "puis tu le renvoies quand c'est fini.",
                [("📚  Vérifier les pubs", "Sur le portail d'abord : Réglages → Base complète → « ⬇ Carte → PC ».\n"
                  "Le fichier téléchargé est trouvé tout seul.", lambda: self.ouvrir_zip()),
                 ("🎙  Ajouter des pubs depuis des captations", "Sur le portail d'abord : Capter → Fichiers de captation → "
                  "« ⬇ Tout télécharger » (et la base comme au-dessus).", lambda: self.ouvrir_captation())])
        self.lbl_acc_envoi = ttk.Label(a, text="", foreground='#c42b1c', font=('Segoe UI', 11, 'bold'))
        self.lbl_acc_envoi.pack(anchor='w', pady=(10, 0))
        self.btn_acc_envoi = ttk.Button(a, text="📂  Montrer le fichier à renvoyer au portail", command=lambda: ouvrir_explorateur(DOSSIER_ENVOI))
        self.btn_acc_reprendre = ttk.Button(a, text="↩  Revenir à la base ouverte", command=lambda: self.montrer('travail'))

    def montrer(self, page):
        """'accueil' ou 'travail' (la liste des pubs)"""
        self.page = page
        if page == 'accueil':
            self.stop()
            self.arreter_revue()
            self.barre.pack_forget()
            self.bandeau.pack_forget()
            self.corps.pack_forget()
            self.page_acc.pack(fill='both', expand=True, before=self.statut)
            self.maj_accueil()
            self.dire("Choisis ton mode : carte SD dans le PC (à gauche) ou portail (à droite).")
        else:
            self.page_acc.pack_forget()
            self.barre.pack(fill='x', before=self.statut)
            self.bandeau.pack(fill='x', before=self.statut)
            self.corps.pack(fill='both', expand=True, padx=12, pady=(0, 6), before=self.statut)
        self.etat_boutons()

    def chiffres(self):
        """le meme resume partout (accueil, bandeau, carte, portail)"""
        if not self.bib:
            return ""
        toutes = [p for p in self.bib.pubs.values() if p.type == PUB]
        nv = sum(p.a_verifier for p in self.bib.pubs.values())      # compte comme la carte (base::nb_a_verifier)
        det = sum(p.vus for p in toutes)                             # comme base::nb_detections
        return "%d pubs  ·  %d à vérifier  ·  %d détections" % (len(toutes), nv, det)

    def maj_accueil(self):
        if self.bib:
            mode = ("carte SD %s" % (os.path.splitdrive(self.bib.dossier)[0] or racine_sd(self.bib.dossier))) if not self.mode_zip \
                else "base du portail"
            self.lbl_acc_chiffres.configure(text="Ouvert : %s  ·  %s" % (mode, self.chiffres()))
            self.btn_acc_reprendre.pack(anchor='w', pady=(8, 0))
        else:
            self.lbl_acc_chiffres.configure(text="Rien d'ouvert pour l'instant. " + VERSION)
            self.btn_acc_reprendre.pack_forget()
        if os.path.isfile(MARQUE_ENVOI):
            self.lbl_acc_envoi.configure(text="⚠  La base corrigée n'a pas encore été renvoyée au portail (« ⬆ PC → carte »).")
            self.btn_acc_envoi.pack(anchor='w', pady=(6, 0))
        else:
            self.lbl_acc_envoi.configure(text="")
            self.btn_acc_envoi.pack_forget()

    def construire_corps(self):
        tk, ttk = self.tk, self.ttk
        corps = self.corps = ttk.PanedWindow(self.r, orient='horizontal')

        g = ttk.Frame(corps)
        cols = ('num', 'nom', 'chaine', 'duree', 'vus', 'verif', 'son', 'appris')
        titres = {'num': "N°", 'nom': "Nom", 'chaine': "Chaîne", 'duree': "Durée", 'vus': "Passages",
                  'verif': "État", 'son': "Son", 'appris': "Apprise le"}
        larg = {'num': 48, 'nom': 200, 'chaine': 110, 'duree': 58, 'vus': 72, 'verif': 100, 'son': 44, 'appris': 128}
        self.liste = ttk.Treeview(g, columns=cols, show='headings', selectmode='browse')
        for c in cols:
            self.liste.heading(c, text=titres[c], command=lambda c=c: self.trier(c))
            self.liste.column(c, width=larg[c], minwidth=40,
                              anchor='w' if c in ('nom', 'chaine', 'appris') else 'center', stretch=c == 'nom')
        self.liste.tag_configure('verif', background=self.couleurs()['verif'])
        sb = ttk.Scrollbar(g, orient='vertical', command=self.liste.yview)
        self.liste.configure(yscrollcommand=sb.set)
        self.liste.pack(side='left', fill='both', expand=True)
        sb.pack(side='left', fill='y')
        corps.add(g, weight=3)
        self.liste.bind('<<TreeviewSelect>>', self.choix)
        self.liste.bind('<Button-3>', self.clic_droit)
        self.liste.bind('<Double-1>', lambda e: self.ecouter(False))

        d = ttk.Frame(corps, padding=(16, 4, 4, 4))
        corps.add(d, weight=2)
        self.lbl_titre = ttk.Label(d, text="Choisis une pub dans la liste", style='Titre.TLabel')
        self.lbl_titre.pack(anchor='w')
        self.lbl_details = ttk.Label(d, text="", style='Sous.TLabel', justify='left')
        self.lbl_details.pack(anchor='w', pady=(2, 12))

        fiche = ttk.LabelFrame(d, text=" Fiche ", padding=10)
        fiche.pack(fill='x')
        ttk.Label(fiche, text="Nom").grid(row=0, column=0, sticky='w')
        self.var_nom = tk.StringVar()
        self.ent_nom = ttk.Entry(fiche, textvariable=self.var_nom)
        self.ent_nom.grid(row=0, column=1, sticky='we', padx=8, pady=3)
        ttk.Label(fiche, text="Chaîne").grid(row=1, column=0, sticky='w')
        self.var_chaine = tk.StringVar()
        self.cmb = ttk.Combobox(fiche, textvariable=self.var_chaine, values=CHAINES, state='readonly')
        self.cmb.grid(row=1, column=1, sticky='we', padx=8, pady=3)
        self.cmb.bind('<<ComboboxSelected>>', lambda e: self.appliquer_fiche())
        ttk.Button(fiche, text="Appliquer", command=self.appliquer_fiche).grid(row=0, column=2, rowspan=2, sticky='ns')
        fiche.columnconfigure(1, weight=1)
        self.ent_nom.bind('<Return>', lambda e: (self.appliquer_fiche(), 'break')[1])

        son = ttk.LabelFrame(d, text=" Son ", padding=10)
        son.pack(fill='x', pady=12)
        self.onde = tk.Canvas(son, height=120, bg=self.couleurs()['fond'], highlightthickness=0)
        self.onde.pack(fill='x')
        self.onde.bind('<Button-1>', self.onde_appui)
        self.onde.bind('<B1-Motion>', self.onde_glisse)
        self.onde.bind('<ButtonRelease-1>', lambda e: setattr(self, 'tire', None))
        self.onde.bind('<Button-3>', self.onde_menu)
        self.onde.bind('<Motion>', self.onde_survol)
        self.onde.bind('<Configure>', lambda e: (self.dessiner(), self.curseur(self.position() if (self.jeu_t0 or self.tete) else None)))
        self.tire = None
        self.menu_onde = tk.Menu(self.r, tearoff=0)
        self.menu_onde.add_command(label="Écouter d'ici", command=lambda: self.jouer(self.t_menu, self.duree_son()))
        self.menu_onde.add_command(label="Début du passage ici", command=lambda: self.poser(0, self.t_menu))
        self.menu_onde.add_command(label="Fin du passage ici", command=lambda: self.poser(1, self.t_menu))
        self.t_menu = 0.0
        ttk.Label(son, text="Clic : aller à cet endroit · glisser le trait jaune / rouge : début / fin",
                  style='Sous.TLabel').pack(anchor='w', pady=(4, 6))
        # lecteur : lecture / pause (reprend a la tete de lecture), stop, temps
        lj = ttk.Frame(son)
        lj.pack(fill='x')
        self.btn['lire'] = ttk.Button(lj, text="▶  Lire", width=10, command=self.basculer_son,
                                      style='Accent.TButton' if self.sv else 'TButton')
        self.btn['lire'].pack(side='left')
        ttk.Button(lj, text="■", width=3, command=self.stop_debut).pack(side='left', padx=4)
        ttk.Button(lj, text="▶  Passage gardé", command=lambda: self.ecouter(True)).pack(side='left', padx=(4, 0))
        self.lbl_temps = ttk.Label(lj, text="0:00.0 / 0:00.0", font=('Consolas', 11))
        self.lbl_temps.pack(side='right')
        self.lbl_gain = ttk.Label(son, text="", style='Sous.TLabel')
        self.lbl_gain.pack(anchor='e')
        # decoupe : poser les marques en ecoutant, verifier chaque coupure
        self.cad_dec = ttk.Frame(son)                    # v0.9.56 : outils de decoupe replies
        self.btn_dec = ttk.Button(son, text="✂  Découper cette pub (ne garder qu'un passage)…", command=self.basculer_decoupe)
        self.btn_dec.pack(anchor='w', pady=(10, 0))
        lm = ttk.Frame(self.cad_dec)
        lm.pack(fill='x', pady=(10, 0))
        ttk.Button(lm, text="Début ici (D)", command=lambda: self.marque(0)).pack(side='left', fill='x', expand=True)
        ttk.Button(lm, text="Fin ici (F)", command=lambda: self.marque(1)).pack(side='left', fill='x', expand=True, padx=(6, 0))
        lv = ttk.Frame(self.cad_dec)
        lv.pack(fill='x', pady=(6, 0))
        ttk.Label(lv, text="Vérifier la coupure :", style='Sous.TLabel').pack(side='left')
        ttk.Button(lv, text="▶ au début", command=lambda: self.verifier_coupure(0)).pack(side='left', padx=6)
        ttk.Button(lv, text="▶ à la fin", command=lambda: self.verifier_coupure(1)).pack(side='left')
        lc = ttk.Frame(self.cad_dec)
        lc.pack(fill='x', pady=(10, 0))
        ttk.Label(lc, text="Garder de").pack(side='left')
        self.var_deb, self.var_fin = tk.DoubleVar(), tk.DoubleVar()
        self.sp_deb = ttk.Spinbox(lc, from_=0, to=999, increment=0.1, textvariable=self.var_deb, width=7,
                                  command=self.lire_selection)
        self.sp_deb.pack(side='left', padx=6)
        ttk.Label(lc, text="à").pack(side='left')
        self.sp_fin = ttk.Spinbox(lc, from_=0, to=999, increment=0.1, textvariable=self.var_fin, width=7,
                                  command=self.lire_selection)
        self.sp_fin.pack(side='left', padx=6)
        ttk.Label(lc, text="s").pack(side='left')
        ttk.Button(lc, text="✂  Découper", command=self.decouper).pack(side='right')
        for w in (self.sp_deb, self.sp_fin):
            w.bind('<FocusOut>', lambda e: self.lire_selection())
            w.bind('<Return>', lambda e: (self.lire_selection(), 'break')[1])

        act = ttk.Frame(d)
        act.pack(fill='x', pady=(4, 0))
        self.btn['ok'] = ttk.Button(act, text="✓  OK, c'est une pub", command=self.ok,
                                    style='Accent.TButton' if self.sv else 'TButton')
        self.btn['ok'].pack(side='left', fill='x', expand=True)
        self.btn['suivante'] = ttk.Button(act, text="Passer  →", command=self.suivante)
        self.btn['suivante'].pack(side='left', padx=6)
        self.btn['suppr'] = ttk.Button(act, text="Supprimer", command=self.supprimer)
        self.btn['suppr'].pack(side='left')

    def basculer_decoupe(self):
        if self.cad_dec.winfo_ismapped():
            self.cad_dec.pack_forget()
            self.btn_dec.configure(text="✂  Découper cette pub (ne garder qu'un passage)…")
        else:
            self.cad_dec.pack(fill='x', after=self.btn_dec)
            self.btn_dec.configure(text="✂  Fermer la découpe")

    def construire_statut(self):
        ttk = self.ttk
        s = self.statut = ttk.Frame(self.r)
        s.pack(fill='x', side='bottom')
        ttk.Separator(s).pack(fill='x')
        self.lbl_statut = ttk.Label(s, text="", style='Statut.TLabel')
        self.lbl_statut.pack(side='left')
        self.lbl_modif = ttk.Label(s, text="", style='Statut.TLabel', foreground='#c42b1c')
        self.lbl_modif.pack(side='right')
        self.lbl_compte = ttk.Label(s, text="", style='Statut.TLabel')
        self.lbl_compte.pack(side='right')

    def raccourcis(self):
        r = self.r

        def hors_saisie(f):
            def g(ev):
                if r.focus_get() in (self.ent_nom, self.ent_cherche, self.sp_deb, self.sp_fin, self.cmb):
                    return None
                f()
                return 'break'
            return g
        r.bind('<space>', hors_saisie(self.basculer_son))
        r.bind('<Return>', hors_saisie(self.ok))
        r.bind('<Delete>', hors_saisie(self.supprimer))
        r.bind('<KeyPress-n>', hors_saisie(self.suivante))
        r.bind('<KeyPress-d>', hors_saisie(lambda: self.marque(0)))
        r.bind('<KeyPress-f>', hors_saisie(lambda: self.marque(1)))
        r.bind('<Left>', hors_saisie(lambda: self.deplacer(-2)))
        r.bind('<Right>', hors_saisie(lambda: self.deplacer(2)))
        r.bind('<Escape>', lambda e: self.stop())
        r.bind('<F2>', lambda e: self.renommer())
        r.bind('<F5>', lambda e: self.relire())
        r.bind('<Control-s>', lambda e: self.enregistrer())
        r.bind('<Control-o>', lambda e: self.ouvrir_zip())
        r.bind('<Control-i>', lambda e: self.ouvrir_captation())
        r.bind('<Control-z>', lambda e: self.annuler())
        r.bind('<Control-r>', lambda e: self.commencer_revue())
        r.bind('<Control-f>', lambda e: (self.ent_cherche.focus_set(), 'break')[1])

    # ================================================================ outils
    def dire(self, t):
        self.lbl_statut.configure(text=t)

    @staticmethod
    def date(s):
        return time.strftime('%d/%m/%Y %H:%M', time.localtime(s)) if s else "—"

    @staticmethod
    def fmt(s):
        s = int(round(s))
        return "%d:%02d" % (s // 60, s % 60)

    def etat_boutons(self):
        ouvert = self.bib is not None
        m = bool(ouvert and self.bib.modifie)
        sd = ouvert and not self.mode_zip
        nv = sum(1 for p in self.bib.pubs.values() if p.type == PUB and p.a_verifier) if ouvert else 0
        self.btn['sauver'].configure(text="✅  Enregistrer sur la carte" if sd else "✅  Terminer : préparer l'envoi",
                                     state='normal' if m else 'disabled')
        self.btn['annuler'].configure(state='normal' if self.annuler_pile else 'disabled')
        if self.revue:
            self.btn['revue'].configure(text="■  Arrêter (%d restantes)" % nv, command=self.arreter_revue, state='normal')
        else:
            self.btn['revue'].configure(text="⚠  Vérifier les pubs (%d)" % nv, command=self.commencer_revue,
                                        state='normal' if nv else 'disabled')
        if sd:
            self.btn['ejecter'].pack(side='left', padx=(0, 8), ipady=4, after=self.btn['sauver'])
        else:
            self.btn['ejecter'].pack_forget()
        for k in ('ok', 'suivante', 'suppr'):
            self.btn[k].configure(state='normal' if ouvert else 'disabled')
        self.lbl_modif.configure(text="●  Modifications non enregistrées" if m else "")
        if ouvert:
            if sd:
                self.lbl_mode.configure(text="💾  MODE 1 · carte SD %s · les changements sont écrits sur la carte"
                                        % (os.path.splitdrive(self.bib.dossier)[0] or racine_sd(self.bib.dossier)))
            else:
                q = (" du " + time.strftime('%d/%m %H:%M', time.localtime(self.zip_date))) if self.zip_date else ""
                self.lbl_mode.configure(text="🌐  MODE 2 · base du portail%s · à renvoyer au portail à la fin" % q)
            self.lbl_chiffres.configure(text=self.chiffres())
        titre = "Détecteur de pub " + VERSION
        if ouvert:
            titre += " — " + ("carte SD" if sd else "base du portail") + (" *" if m else "")
        self.r.title(titre)

    def photo(self):
        """avant chaque modification : pour Ctrl+Z"""
        self.annuler_pile.append((self.bib.instantane(), self.pub.id if self.pub else None))
        del self.annuler_pile[:-20]

    def accueil(self):
        self.montrer('accueil')

    # ================================================================ liste
    def valeurs(self, p):
        return (p.numero, p.nom, CHAINES[p.chaine] if p.chaine else "—", self.fmt(p.duree_s), p.vus,
                "⚠ à vérifier" if p.a_verifier else "✓ OK", "♪" if self.bib.son_present(p) else "—", self.date(p.appris_le))

    def cle(self, p):
        return {'num': p.numero, 'nom': p.nom.lower(), 'chaine': CHAINES[p.chaine], 'duree': p.duree_s, 'vus': p.vus,
                'verif': p.a_verifier, 'son': self.bib.son_present(p), 'appris': p.appris_le}[self.tri['col']]

    def visibles(self):
        b = self.bib
        if not b:
            return []
        f, mot = self.var_filtre.get(), self.var_cherche.get().strip().lower()
        ps = [p for p in b.pubs.values() if p.type == PUB]
        if f == 'verif':
            ps = [p for p in ps if p.a_verifier]
        elif f == 'ok':
            ps = [p for p in ps if not p.a_verifier]
        if mot:
            ps = [p for p in ps if mot in p.nom.lower() or mot in CHAINES[p.chaine].lower() or mot == str(p.numero)]
        ps.sort(key=self.cle, reverse=self.tri['inverse'])
        return ps

    def remplir(self, garder=-1):
        if garder == -1:
            garder = self.pub.id if self.pub else None
        self.liste.delete(*self.liste.get_children())
        if not self.bib:
            return
        for p in self.visibles():
            self.liste.insert('', 'end', iid=str(p.id), values=self.valeurs(p), tags=('verif',) if p.a_verifier else ())
        toutes = [p for p in self.bib.pubs.values() if p.type == PUB]
        nv = sum(p.a_verifier for p in toutes)
        self.lbl_compte.configure(text="mémoire de la base %.0f %%" % (100.0 * len(self.bib.entrees) / CAP))
        if garder is not None and self.liste.exists(str(garder)):
            self.liste.selection_set(str(garder))
            self.liste.see(str(garder))
        elif self.pub and not self.liste.exists(str(self.pub.id)):
            self.afficher(None)
        self.maj_revue()
        self.etat_boutons()

    def trier(self, c):
        self.tri['inverse'] = (not self.tri['inverse']) if self.tri['col'] == c else c in ('num', 'vus', 'appris', 'duree')
        self.tri['col'] = c
        self.remplir()

    def choix(self, _=None):
        s = self.liste.selection()
        p = self.bib.pubs.get(int(s[0])) if (self.bib and s) else None
        if p is not self.pub:
            self.afficher(p)
            if p and (self.var_auto.get() or self.revue):
                self.ecouter(False)

    def selectionner(self, iid):
        if iid and self.liste.exists(iid):
            self.liste.selection_set(iid)
            self.liste.focus(iid)
            self.liste.see(iid)
            self.choix()                             # tout de suite (l'evenement de la liste arrive plus tard)
            return True
        return False

    def suivante(self, depuis=None):
        enf = list(self.liste.get_children())
        if not enf:
            self.afficher(None)
            self.fin_revue_si_vide()
            return
        cur = str(depuis if depuis is not None else (self.pub.id if self.pub else ''))
        if cur in enf:
            i = enf.index(cur)
            cible = enf[i + 1] if i + 1 < len(enf) else (enf[0] if self.revue else enf[i])
        else:
            cible = enf[0]
        if cible == cur and not self.revue:
            return
        self.selectionner(cible)

    def clic_droit(self, ev):
        iid = self.liste.identify_row(ev.y)
        if iid:
            self.selectionner(iid)
            self.menu_ctx.tk_popup(ev.x_root, ev.y_root)

    # ================================================================ fiche
    def afficher(self, p):
        self.stop()
        self.pub = p
        if not p:
            self.lbl_titre.configure(text="Choisis une pub dans la liste")
            self.lbl_details.configure(text="")
            self.var_nom.set("")
            self.var_chaine.set("")
            self.son = None
            self.dessiner()
            return
        self.lbl_titre.configure(text="Pub n°%d  ·  %s" % (p.numero, p.nom))
        self.lbl_details.configure(text="%s  ·  %d empreintes  ·  reconnue %d fois  ·  %s\nApprise le %s  ·  vue le %s  ·  origine : %s"
                                   % (self.fmt(p.duree_s), p.nb, p.vus, "à vérifier" if p.a_verifier else "OK",
                                      self.date(p.appris_le), self.date(p.vu_le), ORIGINES.get(p.origine, '?')))
        self.var_nom.set(p.nom)
        self.var_chaine.set(CHAINES[p.chaine])
        self.recharger_son()

    def recharger_son(self):
        p = self.pub
        self.son = self.bib.echantillons(p) if p else None
        self.gain = gain_auto(self.son) if (self.son and self.var_vol.get()) else 1.0
        duree = len(self.son) / FS_WAV if self.son else float(p.duree_s if p else 0)
        self.sel = [0.0, round(duree, 1)]
        self.tete = 0.0
        self.maj_temps()
        self.var_deb.set(0.0)
        self.var_fin.set(round(duree, 1))
        self.lbl_gain.configure(text=("Volume ×%.1f" % self.gain) if self.son else ("Pas de son sur la carte" if p else ""))
        self.dessiner()

    def dessiner(self):
        c = self.couleurs()
        o = self.onde
        o.delete('all')
        l = o.winfo_width() if o.winfo_width() > 10 else 440
        h = int(o.cget('height'))
        if not self.son:
            o.create_text(l // 2, h // 2, text="Pas de son" if self.pub else "", fill=c['texte'])
            return
        son, n, g = self.son, len(self.son), self.gain
        pas, mid = max(1, n // l), h / 2
        duree = n / FS_WAV
        xd, xf = self.sel[0] / duree * l, self.sel[1] / duree * l
        for x in range(l):
            bout = son[x * n // l: x * n // l + pas]
            if not bout:
                break
            a = min(1.0, max(abs(min(bout)), abs(max(bout))) * g / 32768.0)
            o.create_line(x, mid - a * (mid - 6), x, mid + a * (mid - 6) + 1, fill=c['onde'] if xd <= x <= xf else c['hors'])
        o.create_line(xd, 0, xd, h, fill=c['debut'], width=2)
        o.create_line(xf, 0, xf, h, fill=c['fin'], width=2)
        o.create_text(4, 3, text="0:00", fill=c['texte'], anchor='nw')
        o.create_text(l - 4, 3, text=self.fmt(duree), fill=c['texte'], anchor='ne')

    def curseur(self, t):
        self.onde.delete('curseur')
        if t is None or not self.son:
            return
        l = self.onde.winfo_width() if self.onde.winfo_width() > 10 else 440
        xc = t / (len(self.son) / FS_WAV) * l
        self.onde.create_line(xc, 0, xc, int(self.onde.cget('height')), fill=self.couleurs()['curseur'], tags='curseur')

    def lire_selection(self):
        try:
            d, f = float(self.var_deb.get()), float(self.var_fin.get())
        except (self.tk.TclError, ValueError):
            return
        self.sel = [max(0.0, d), max(0.0, f)]
        self.dessiner()
        if self.jeu_t0 is not None or self.tete:
            self.curseur(self.position())

    def t_de_x(self, x):
        l = max(1, self.onde.winfo_width())
        return round(max(0, min(l, x)) / l * self.duree_son(), 1)

    def x_de_t(self, t):
        return t / self.duree_son() * self.onde.winfo_width() if self.son else 0

    def pres_marque(self, x):
        """0 ou 1 si x est sur le trait de debut / fin (8 px), sinon None"""
        for k in (0, 1):
            if abs(self.x_de_t(self.sel[k]) - x) <= 8:
                return k
        return None

    def onde_appui(self, ev):
        if not self.son:
            return
        self.tire = self.pres_marque(ev.x)
        if self.tire is None:                       # clic ailleurs : la tete de lecture va la
            t = self.t_de_x(ev.x)
            if self.jeu_t0 is not None:
                self.jouer(t, self.duree_son())
            else:
                self.tete = t
                self.curseur(t)
                self.maj_temps()

    def onde_glisse(self, ev):
        if self.son and self.tire is not None:
            self.poser(self.tire, self.t_de_x(ev.x))

    def onde_survol(self, ev):
        self.onde.configure(cursor='sb_h_double_arrow' if (self.son and self.pres_marque(ev.x) is not None) else '')

    def onde_menu(self, ev):
        if self.son:
            self.t_menu = self.t_de_x(ev.x)
            self.menu_onde.tk_popup(ev.x_root, ev.y_root)

    def poser(self, quoi, t):
        if quoi == 0:
            t = min(t, self.sel[1])
        else:
            t = max(t, self.sel[0])
        (self.var_deb if quoi == 0 else self.var_fin).set(round(t, 1))
        self.lire_selection()

    # ================================================================ son
    def duree_son(self):
        return len(self.son) / FS_WAV if self.son else 0.0

    def ecouter(self, passage=False):
        if passage:
            self.jouer(self.sel[0], self.sel[1])
        else:
            self.jouer(0.0, self.duree_son())

    def jouer(self, d, f):
        if not self.son:
            if self.pub:
                self.dire("Pas de son sur la carte pour la pub n°%d." % self.pub.numero)
            return
        d = max(0.0, min(d, self.duree_son()))
        f = max(d, min(f, self.duree_son()))
        bout = self.son[int(d * FS_WAV):int(f * FS_WAV)]
        if len(bout) < FS_WAV // 10:
            return
        self.lecteur.jouer(amplifier(bout, self.gain) if self.gain != 1.0 else bout)
        self.jeu_gen += 1
        self.jeu_t0, self.jeu_debut, self.jeu_duree = time.time(), d, len(bout) / FS_WAV
        self.btn['lire'].configure(text="❚❚  Pause")
        self.avancer(self.jeu_gen)

    def marque(self, quoi):
        """pose le debut (0) ou la fin (1) du passage a garder a la tete de lecture"""
        if not self.son:
            return
        t = round(self.position(), 1)
        self.poser(quoi, t)
        self.dire("%s du passage : %s" % ("Début" if quoi == 0 else "Fin", self.fmt_fin(t)))

    def verifier_coupure(self, quoi):
        """ecoute juste autour d'une coupure : on entend si elle tombe au bon endroit"""
        d, f = self.sel
        if quoi == 0:
            self.jouer(d, d + 3.0)
        else:
            self.jouer(max(d, f - 3.0), f)

    def deplacer(self, ds):
        """fleches gauche / droite : la tete de lecture recule / avance de 2 s"""
        if not self.son:
            return
        en_cours = self.jeu_t0 is not None
        t = max(0.0, min(self.duree_son(), self.position() + ds))
        self.stop()
        self.tete = t
        self.maj_temps()
        self.curseur(t)
        if en_cours:
            self.jouer(t, self.duree_son())

    def position(self):
        if self.jeu_t0 is not None:
            return min(self.jeu_debut + time.time() - self.jeu_t0, self.jeu_debut + self.jeu_duree)
        return self.tete

    @staticmethod
    def fmt_fin(s):
        return "%d:%04.1f" % (int(s) // 60, s - int(s) // 60 * 60)

    def maj_temps(self):
        self.lbl_temps.configure(text="%s / %s" % (self.fmt_fin(self.position()), self.fmt_fin(self.duree_son())))

    def avancer(self, gen):
        if self.jeu_t0 is None or gen != self.jeu_gen:
            return
        e = time.time() - self.jeu_t0
        if e > self.jeu_duree:
            fin = self.jeu_debut + self.jeu_duree
            self.jeu_t0 = None
            self.tete = 0.0 if fin >= self.duree_son() - 0.05 else fin
            self.btn['lire'].configure(text="▶  Lire")
            self.curseur(self.tete if self.tete else None)
            self.maj_temps()
            return
        self.curseur(self.jeu_debut + e)
        self.maj_temps()
        self.r.after(40, lambda: self.avancer(gen))

    def stop(self):
        """pause : le son s'arrete, la tete de lecture reste ou elle est"""
        if self.jeu_t0 is not None:
            self.tete = self.position()
        self.lecteur.arreter()
        self.jeu_t0 = None
        if 'lire' in self.btn:
            self.btn['lire'].configure(text="▶  Lire")

    def stop_debut(self):
        self.stop()
        self.tete = 0.0
        self.curseur(None)
        self.maj_temps()

    def basculer_son(self):
        if self.jeu_t0 is not None:
            self.stop()
            self.curseur(self.tete)
            self.maj_temps()
        else:
            t = self.tete if self.tete < self.duree_son() - 0.2 else 0.0
            self.jouer(t, self.duree_son())

    # ================================================================ actions
    def ok(self):
        p = self.pub
        if not p:
            return
        self.photo()
        self.bib.marquer_ok(p, True)
        self.dire("Pub n°%d : OK." % p.numero)
        self.apres_action(p)

    def a_verifier(self):
        p = self.pub
        if not p:
            return
        self.photo()
        self.bib.marquer_ok(p, False)
        self.dire("Pub n°%d : remise « à vérifier »." % p.numero)
        self.remplir()
        self.afficher(p)

    def apres_action(self, p):
        """la liste change (filtre) : on va a la pub qui suivait"""
        enf = list(self.liste.get_children())
        i = enf.index(str(p.id)) if str(p.id) in enf else 0
        self.remplir(garder=None)
        enf = list(self.liste.get_children())
        if str(p.id) in enf:
            self.suivante(p.id)
        elif enf:
            self.selectionner(enf[min(i, len(enf) - 1)])
        else:
            self.afficher(None)
            self.fin_revue_si_vide()

    def renommer(self):
        p = self.pub
        if not p:
            return
        nom = self.sd.askstring("Renommer", "Nom de la pub n°%d :" % p.numero, initialvalue=p.nom, parent=self.r)
        if nom is None or not nom.strip():
            return
        self.photo()
        self.bib.renommer(p, nom, -1)
        self.remplir()
        self.afficher(p)
        self.dire("Pub n°%d renommée." % p.numero)

    def changer_chaine(self, i):
        p = self.pub
        if not p:
            return
        self.photo()
        self.bib.renommer(p, "", i)
        self.remplir()
        self.afficher(p)

    def appliquer_fiche(self):
        p = self.pub
        if not p:
            return
        try:
            ch = CHAINES.index(self.var_chaine.get())
        except ValueError:
            ch = -1
        if self.var_nom.get().strip() == p.nom and ch == p.chaine:
            return
        self.photo()
        self.bib.renommer(p, self.var_nom.get(), ch)
        self.remplir()
        self.afficher(p)
        self.dire("Pub n°%d : fiche modifiée." % p.numero)

    def supprimer(self):
        p = self.pub
        if not p:
            return
        if not self.revue and not self.mb.askyesno("Supprimer", "Supprimer la pub n°%d « %s » et son son ?" % (p.numero, p.nom)):
            return
        self.photo()
        enf = list(self.liste.get_children())
        i = enf.index(str(p.id)) if str(p.id) in enf else 0
        self.bib.supprimer(p)
        self.pub = None
        self.remplir(garder=None)
        enf = list(self.liste.get_children())
        if enf:
            self.selectionner(enf[min(i, len(enf) - 1)])
        else:
            self.afficher(None)
            self.fin_revue_si_vide()
        self.dire("Pub n°%d supprimée (Ctrl + Z pour annuler)." % p.numero)

    def doublons(self):
        """B2 : pubs qui sont la meme (plusieurs passages) -> fusion des codes en une seule"""
        if not self.bib:
            self.dire("Ouvre d'abord la carte SD.")
            return
        tk, ttk = self.tk, self.ttk
        self.dire("Recherche des doublons…")
        self.r.update_idletasks()
        fen = tk.Toplevel(self.r)
        fen.title("Doublons et fusion de passages")
        fen.geometry("900x480")
        fen.minsize(700, 360)
        ttk.Label(fen, text="Ces pubs se ressemblent (mêmes empreintes, décalées dans le temps). Fusionner garde la plus complète "
                  "et lui ajoute les codes de l'autre : reconnaissance plus robuste, une seule pub dans la liste.\n"
                  "À 100 % c'est la même pub. En dessous, écoute A et B avant de décider.",
                  wraplength=860, justify="left").pack(anchor="w", padx=10, pady=(10, 4))
        # v0.9.56 : les boutons sont places AVANT la liste (en bas), sinon ils sortaient de la fenetre sous Windows
        bt = ttk.Frame(fen)
        bt.pack(side="bottom", fill="x", padx=10, pady=10)
        etat = ttk.Label(fen, text="")
        etat.pack(side="bottom", anchor="w", padx=10)
        cols = ("a", "b", "sim", "dec")
        arbre = ttk.Treeview(fen, columns=cols, show="headings", selectmode="extended", height=6)
        for c, t, w in (("a", "Pub A", 330), ("b", "Pub B", 330), ("sim", "Ressemblance", 90), ("dec", "Décalage", 70)):
            arbre.heading(c, text=t)
            arbre.column(c, width=w, anchor="w")
        arbre.pack(fill="both", expand=True, padx=10, pady=4)
        paires = {}
        sims = {}

        def nom(p):
            return "n°%d  %s  (%d s%s)" % (p.numero, p.nom, p.duree_s, ", ♪" if self.bib.son_present(p) else "")

        def remplir():
            arbre.delete(*arbre.get_children())
            paires.clear()
            sims.clear()
            for i, (a, b, n, pct, dec) in enumerate(self.bib.doublons()):
                if a in self.bib.pubs and b in self.bib.pubs:
                    paires[str(i)] = (a, b)
                    sims[str(i)] = pct
                    arbre.insert("", "end", iid=str(i), values=(nom(self.bib.pubs[a]), nom(self.bib.pubs[b]), "%d %%" % pct, "%+.1f s" % dec))
            etat.config(text="%d paire(s) trouvée(s)." % len(paires) if paires else "Aucun doublon.")

        def qui_garde(a, b):
            pa, pb = self.bib.pubs[a], self.bib.pubs[b]
            ka = (self.bib.son_present(pa), pa.duree_s, -pa.numero)
            kb = (self.bib.son_present(pb), pb.duree_s, -pb.numero)
            return (pa, pb) if ka >= kb else (pb, pa)

        def fusionner(ids):
            fait = 0
            self.photo()
            for i in ids:
                a, b = paires[i]
                if a not in self.bib.pubs or b not in self.bib.pubs:
                    continue
                g, x = qui_garde(a, b)
                try:
                    self.bib.fusionner(g, x)
                    fait += 1
                except ValueError as e:
                    self.mb.showerror("Fusion", str(e))
                    break
            self.pub = None
            self.remplir(garder=None)
            remplir()
            self.dire("%d fusion(s) faite(s) (Ctrl + Z pour annuler). N'oublie pas d'enregistrer." % fait)

        def ecouter(cote):
            sel = arbre.selection()
            if not sel or sel[0] not in paires:
                self.mb.showinfo("Écouter", "Choisis d'abord une ligne.", parent=fen)
                return
            ident = paires[sel[0]][cote]
            if not self.liste.exists(str(ident)):         # filtre ou recherche en cours : tout remontrer
                self.var_filtre.set('toutes')
                self.var_cherche.set('')
                self.remplir()
            self.selectionner(str(ident))
            self.ecouter(False)

        def identiques():
            ids = [i for i in paires if sims.get(i, 0) >= 90]
            if not ids:
                self.mb.showinfo("Fusionner", "Aucune paire à 90 % ou plus.", parent=fen)
                return
            fusionner(ids)

        ttk.Button(bt, text="▶  Écouter A", command=lambda: ecouter(0)).pack(side="left", ipady=4)
        ttk.Button(bt, text="▶  Écouter B", command=lambda: ecouter(1)).pack(side="left", padx=(6, 18), ipady=4)
        ttk.Button(bt, text="🔗  Fusionner la ligne choisie", command=lambda: arbre.selection() and fusionner(list(arbre.selection())),
                   style='Accent.TButton' if self.sv else 'TButton').pack(side="left", ipady=4)
        ttk.Button(bt, text="🔗  Fusionner toutes les identiques (90 % et plus)", command=identiques).pack(side="left", padx=8, ipady=4)
        ttk.Button(bt, text="Fermer", command=fen.destroy).pack(side="right", ipady=4)
        remplir()
        self.dire("")

    def decouper(self):
        p = self.pub
        if not p:
            return
        self.lire_selection()
        d, f = self.sel
        if f - d < 5:
            self.mb.showwarning("Découper", "Le passage gardé doit durer au moins 5 secondes.")
            return
        if not self.mb.askyesno("Découper", "Ne garder que %.1f s → %.1f s de la pub n°%d ?\n"
                                "Le reste des empreintes et du son est retiré." % (d, f, p.numero)):
            return
        self.stop()
        self.photo()
        reste = self.bib.decouper(p, d, f)
        self.remplir(p.id if reste else None)
        self.afficher(p if reste else None)
        self.dire(("Pub n°%d découpée : %s gardées." % (p.numero, self.fmt(f - d))) if reste else
                  "Pub n°%d : plus aucune empreinte dans ce passage, elle a été supprimée." % p.numero)

    def annuler(self):
        if not self.annuler_pile or not self.bib:
            return
        photo, ident = self.annuler_pile.pop()
        self.stop()
        self.bib.revenir(photo)
        self.pub = None
        self.remplir(garder=None)
        enf = self.liste.get_children()
        if not (ident is not None and self.selectionner(str(ident))) and enf:
            self.selectionner(enf[0])
        self.dire("Dernière action annulée.")

    # ================================================================ revue
    def commencer_revue(self):
        if not self.bib:
            return
        if not any(p.a_verifier for p in self.bib.pubs.values() if p.type == PUB):
            self.mb.showinfo("Vérifier", "Aucune pub « à vérifier » : tout est déjà vérifié.")
            return
        self.revue = True
        self.var_filtre.set('verif')
        self.var_cherche.set('')
        self.tri.update(col='num', inverse=False)
        self.remplir(garder=None)
        enf = self.liste.get_children()
        self.pub = None
        if enf:
            self.selectionner(enf[0])
        self.etat_boutons()
        self.dire("Vérification : Entrée = OK c'est une pub · Suppr = supprimer · N = passer · Espace = réécouter.")
        self.liste.focus_set()

    def arreter_revue(self):
        self.revue = False
        if hasattr(self, 'btn'):
            self.etat_boutons()

    def maj_revue(self):
        if not self.revue or not self.bib:
            return
        self.etat_boutons()

    def fin_revue_si_vide(self):
        if self.revue and not any(p.a_verifier for p in self.bib.pubs.values() if p.type == PUB):
            self.arreter_revue()
            self.var_filtre.set('toutes')
            self.remplir(garder=None)
            if self.mb.askyesno("Vérification terminée", "Toutes les pubs sont vérifiées.\n\nEnregistrer maintenant ?"):
                self.enregistrer()

    # ================================================================ fichiers
    def ouvrir(self, chemin=None, zip_date=None, revue=True):
        if self.bib and self.bib.modifie and not self.mb.askyesno(
                "Modifications", "Abandonner les modifications non enregistrées ?"):
            return
        chemin = chemin or self.fd.askdirectory(title="Choisis la carte SD du détecteur (ex. E:\\)")
        if not chemin:
            return
        self.mode_zip = os.path.abspath(chemin).startswith(os.path.abspath(DOSSIER_BASE))
        self.zip_date = zip_date if self.mode_zip else None
        try:
            b = Bibliotheque(chemin)
        except (OSError, ValueError, struct.error) as e:
            self.mb.showerror("Lecture impossible", "%s\n\nChoisis la racine de la carte SD (elle contient le dossier « pub »)." % e)
            return
        self.stop()
        self.arreter_revue()
        self.bib, self.pub, self.annuler_pile = b, None, []
        self.var_filtre.set('toutes')
        self.remplir(garder=None)
        self.afficher(None)
        enf = self.liste.get_children()
        if enf:
            self.selectionner(enf[0])
        nv = sum(p.a_verifier for p in b.pubs.values() if p.type == PUB)
        self.montrer('travail')
        self.dire(("Base du portail ouverte. " if self.mode_zip else "Carte SD lue. ") + (("%d pubs à vérifier." % nv) if nv else "Tout est vérifié."))
        if revue and nv and self.mb.askyesno("Vérifier", "%d pub(s) à vérifier (apprises par le détecteur).\n\nLes vérifier maintenant ?" % nv):
            self.commencer_revue()
        return True

    def relire(self):
        if self.bib:
            self.ouvrir(self.bib.dossier)

    def ouvrir_zip(self, z=None, choisir=False, revue=True):
        """v0.9.54 : la base rapatriee depuis le portail (ZIP) est depliee dans DetecteurPub/base
        (toujours le meme dossier : l'ancienne base de travail est remplacee). La carte SD n'est pas touchee."""
        if self.bib and self.bib.modifie and not self.mb.askyesno(
                "Modifications", "Abandonner les modifications non enregistrées ?"):
            return False
        if os.path.isfile(MARQUE_ENVOI) and not self.mb.askyesno(
                "Base pas encore envoyée ?",
                "La dernière base corrigée (a_envoyer/base_corrigee.zip) n'a peut-être pas été envoyée à la carte.\n\n"
                "Si tu ouvres une nouvelle base de la carte, ces corrections ne seront pas dedans.\n\n"
                "As-tu déjà envoyé base_corrigee.zip à la carte (portail → « PC → carte ») ?"):
            ouvrir_explorateur(DOSSIER_ENVOI)
            return False
        if not z and not choisir:
            z = self.zip_du_portail('base')
            if z == 'deja':
                self.montrer('travail')
                return True
        z = z or self.fd.askopenfilename(title="Base téléchargée depuis le portail (base_pub.zip)", initialdir=dossier_telechargements(),
                                         filetypes=[("Base ZIP", "*.zip"), ("Tous", "*.*")])
        if not z:
            return False
        pub = os.path.join(DOSSIER_BASE, 'pub')
        try:
            with zipfile.ZipFile(z) as arc:
                if arc.testzip() is not None:
                    raise ValueError("archive abîmée (CRC)")
                noms = [i.filename.replace('\\', '/') for i in arc.infolist()]
                if 'base.bin' not in noms:
                    raise ValueError("ce ZIP ne contient pas de base (base.bin) : est-ce bien « Carte → PC » ?")
                for n in noms:
                    if n.startswith('/') or '..' in n.split('/'):
                        raise ValueError("nom refusé : %s" % n)
                os.makedirs(DOSSIER_PC, exist_ok=True)
                vider_dossier(DOSSIER_BASE)
                for info in arc.infolist():
                    n = info.filename.replace('\\', '/')
                    if n == 'base.bin' or n == 'infos.bin' or (n.startswith('audio/') and n.endswith('.wav')):
                        arc.extract(info, pub)
            os.makedirs(os.path.join(pub, 'audio'), exist_ok=True)
        except (OSError, ValueError, zipfile.BadZipFile) as e:
            self.mb.showerror("ZIP illisible", str(e))
            return False
        if os.path.isfile(MARQUE_ENVOI):
            os.remove(MARQUE_ENVOI)
        noter_ouvert(z)
        self.base_zip = z
        self.ouvrir(DOSSIER_BASE, zip_date=os.path.getmtime(z), revue=revue)
        return self.bib is not None

    def zip_du_portail(self, genre):
        """le dernier .zip telecharge depuis le portail ; None = le choisir soi-meme ; 'deja' = c'est la base deja ouverte"""
        z = zip_recent(genre)
        quoi = "la base (« ⬇ Carte → PC »)" if genre == 'base' else "les captations (« ⬇ Tout télécharger »)"
        if not z:
            self.mb.showinfo("Fichier pas trouvé",
                             "Je ne trouve pas %s dans ton dossier Téléchargements.\n\n"
                             "Télécharge-le d'abord sur le portail, ou choisis le fichier toi-même." % quoi)
            return None
        quand = time.strftime('%d/%m à %H:%M', time.localtime(os.path.getmtime(z)))
        if genre == 'base' and self.bib and self.mode_zip and getattr(self, 'base_zip', None) and \
                cle_fichier(z) == cle_fichier(self.base_zip):
            return 'deja'
        if deja_ouvert(z):
            if self.mb.askyesno("Déjà ouvert", "Le dernier fichier téléchargé, « %s » (du %s), a déjà été ouvert.\n\n"
                                "As-tu oublié d'en télécharger un nouveau sur le portail ?\n\n"
                                "Oui : je le choisis moi-même.   Non : je rouvre celui-ci." % (os.path.basename(z), quand)):
                return None
        self.dire("Fichier du portail : %s (du %s)" % (os.path.basename(z), quand))
        return z

    def preparer_envoi(self):
        """v0.9.54 : ZIP a envoyer au portail, toujours au meme endroit (a_envoyer/base_corrigee.zip)"""
        d = self.bib.dossier
        os.makedirs(DOSSIER_ENVOI, exist_ok=True)
        tmp = FICHIER_ENVOI + '.tmp'
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED) as arc:
            for nom in ('base.bin', 'infos.bin'):
                if os.path.isfile(os.path.join(d, nom)):
                    arc.write(os.path.join(d, nom), nom)
            ad = os.path.join(d, 'audio')
            if os.path.isdir(ad):
                for f in sorted(os.listdir(ad)):
                    if f.lower().endswith('.wav') and os.path.getsize(os.path.join(ad, f)) > 0:
                        arc.write(os.path.join(ad, f), 'audio/' + f)
        os.replace(tmp, FICHIER_ENVOI)
        with open(MARQUE_ENVOI, 'w', encoding='utf-8') as f:
            f.write(time.strftime('%Y-%m-%d %H:%M:%S') + " : base_corrigee.zip pas encore envoyée à la carte\n")

    def exporter_zip(self):
        """v0.9.38 : fabrique le ZIP a envoyer au portail (Reglages -> Base complete -> Envoyer)."""
        if not self.bib:
            self.mb.showwarning("Exporter", "Ouvre d'abord une base.")
            return
        if self.bib.modifie:
            if not self.mb.askyesno("Modifications", "Des modifications ne sont pas enregistrées. Enregistrer avant d'exporter ?"):
                return
            self.enregistrer()
        d = self.bib.dossier
        sortie = self.fd.asksaveasfilename(title="ZIP à envoyer à la carte", defaultextension=".zip",
                                           initialfile='base_corrigee_' + time.strftime('%Y%m%d_%H%M') + '.zip',
                                           filetypes=[("Base ZIP", "*.zip")])
        if not sortie:
            return
        try:
            with zipfile.ZipFile(sortie, 'w', zipfile.ZIP_STORED) as arc:
                for nom in ('base.bin', 'infos.bin'):
                    if os.path.isfile(os.path.join(d, nom)):
                        arc.write(os.path.join(d, nom), nom)
                ad = os.path.join(d, 'audio')
                if os.path.isdir(ad):
                    for f in sorted(os.listdir(ad)):
                        if f.lower().endswith('.wav') and os.path.getsize(os.path.join(ad, f)) > 0:
                            arc.write(os.path.join(ad, f), 'audio/' + f)
        except OSError as e:
            self.mb.showerror("Export impossible", str(e))
            return
        self.dire("ZIP prêt (%.1f Mo) : portail → Réglages → Base complète → Envoyer la base corrigée." % (os.path.getsize(sortie) / 1048576))

    def proposer_suppression(self, chemin, raison, parent=None):
        """fichier inexploitable (trop court, illisible...) : propose de le supprimer.
        True = fait (le fichier n'est plus la). v0.9.54 : vraie suppression (plus de dossier _supprimees)."""
        if not self.mb.askyesno("Fichier inexploitable",
                                "« %s » ne peut pas être utilisé :\n%s\n\nLe supprimer ?" % (os.path.basename(chemin), raison),
                                parent=parent or self.r):
            return False
        try:
            os.remove(chemin)
        except OSError as e:
            self.mb.showerror("Supprimer", "Impossible de supprimer : %s" % e, parent=parent or self.r)
            return False
        self.dire("« %s » supprimé." % os.path.basename(chemin))
        return True

    def ouvrir_captation(self, choisir=False):
        """v0.9.54 : captations du portail (ZIP « Tout télécharger ») ou WAV, a decouper en pubs.
        La base est ouverte d'abord (c'est la que vont les pubs ajoutees)."""
        if np is None:
            self.mb.showerror("Captation", "Cette fonction demande numpy.\n\nDans une invite de commandes :   pip install numpy")
            return
        if not self.bib:                             # les pubs trouvees vont dans la base : on l'ouvre d'abord
            self.dire("D'abord la base (les pubs trouvées y sont ajoutées)…")
            if not self.ouvrir_zip(choisir=choisir, revue=False):
                return
        chemins = None
        if not self.mode_zip and not choisir:        # MODE 1 : les captations sont sur la carte SD
            cd = os.path.join(self.bib.dossier, 'captations')
            au = os.path.join(cd, 'auto')
            chemins = [os.path.join(au, n) for n in sorted(os.listdir(au))
                       if n.lower().endswith('.wav') and not n.startswith('_')] if os.path.isdir(au) else []
            chemins += [os.path.join(cd, n) for n in sorted(os.listdir(cd))
                        if n.lower().startswith('cap_') and n.lower().endswith('.wav')] if os.path.isdir(cd) else []
            if not chemins:
                self.mb.showinfo("Captations", "Aucune captation sur la carte SD (dossier pub/captations vide).")
                return
        elif not choisir:                             # MODE 2 : le dernier ZIP de captations telecharge
            z = self.zip_du_portail('cap')
            if z and z != 'deja':
                chemins = [z]
        if not chemins:
            options = {'initialdir': dossier_telechargements()}
            if not self.mode_zip and os.path.isdir(os.path.join(self.bib.dossier, 'captations')):
                options['initialdir'] = os.path.join(self.bib.dossier, 'captations')
            chemins = self.fd.askopenfilenames(title="Captations : ZIP du portail (captations_auto.zip…) ou fichiers WAV",
                                               filetypes=[("ZIP du portail ou WAV", "*.zip *.wav"), ("Tous les fichiers", "*.*")], **options)
        if not chemins:
            return
        zips = [c for c in chemins if c.lower().endswith('.zip')]
        for z in zips:
            noter_ouvert(z)
        if zips:                                     # ZIP : deplie dans DetecteurPub/captations (remplace la fois precedente)
            try:
                vider_dossier(DOSSIER_CAPT)
                for z in zips:
                    with zipfile.ZipFile(z) as arc:
                        for info in arc.infolist():
                            n = info.filename.replace('\\', '/')
                            if '/' in n or n.startswith('.'):
                                continue
                            if n.lower().endswith('.wav') or n == 'index.csv':
                                arc.extract(info, DOSSIER_CAPT)
            except (OSError, zipfile.BadZipFile) as e:
                self.mb.showerror("ZIP illisible", str(e))
                return
            chemins = [os.path.join(DOSSIER_CAPT, n) for n in os.listdir(DOSSIER_CAPT) if n.lower().endswith('.wav')]
            chemins += [c for c in chemins if False]
            if not chemins:
                self.mb.showinfo("Captations", "Aucun fichier WAV dans ce ZIP.")
                return
        chemins = sorted(chemins)
        self.stop()
        cap = None
        while chemins and cap is None:
            try:
                self.r.configure(cursor='watch')
                self.r.update_idletasks()
                cap = Captation(chemins[0])
            except (OSError, ValueError, RuntimeError) as e:
                self.r.configure(cursor='')
                self.proposer_suppression(chemins[0], str(e))     # supprime ou laisse en place ; dans les deux cas, au suivant
                chemins = chemins[1:]
            finally:
                self.r.configure(cursor='')
        if cap is None:
            return
        FenetreCaptation(self, cap, chemins)

    def enregistrer(self):
        b = self.bib
        if not b or not b.modifie:
            return
        self.stop()
        if self.mode_zip:                              # v0.9.54 : enregistre la base de travail et prepare l'envoi
            try:
                b.enregistrer()
                self.preparer_envoi()
            except OSError as e:
                self.mb.showerror("Enregistrement impossible", str(e))
                return
            self.annuler_pile = []
            self.remplir()
            self.dire("Enregistré. À envoyer : %s (%.1f Mo)" % (FICHIER_ENVOI, os.path.getsize(FICHIER_ENVOI) / 1048576))
            self.mb.showinfo("Prêt à envoyer",
                             "La base corrigée est prête :\n%s\n\nSur le portail : Réglages → Base complète →\n"
                             "« ⬆ PC → carte : envoyer la base corrigée », choisis ce fichier, puis « Remplacer la base ».\n\n"
                             "Le dossier va s'ouvrir." % FICHIER_ENVOI)
            ouvrir_explorateur(DOSSIER_ENVOI)
            return
        try:
            sauve = b.enregistrer()
        except OSError as e:
            self.mb.showerror("Écriture impossible", "%s\n\nLa carte SD est-elle protégée en écriture (petit levier sur le côté) ?" % e)
            return
        self.annuler_pile = []
        self.remplir()
        self.dire("Enregistré. Copie de l'ancienne bibliothèque : %s" % sauve)
        if self.mb.askyesno("Enregistré", "La bibliothèque est écrite sur la carte SD.\n\n"
                            "Éjecter la carte SD maintenant ?\n(ensuite : la remettre dans le détecteur éteint, puis le rallumer)"):
            self.ejecter()

    def ejecter(self):
        if not self.bib:
            return
        if self.bib.modifie and not self.mb.askyesno("Éjecter", "Des modifications ne sont pas enregistrées. Éjecter quand même ?"):
            return
        lecteur = os.path.splitdrive(self.bib.dossier)[0]
        if not sys.platform.startswith('win') or not lecteur:
            self.mb.showinfo("Éjecter", "Éjecte la carte SD avec ton système, puis remets-la dans le détecteur éteint.")
            self.bib = None
            self.montrer('accueil')
            return
        self.stop()
        try:
            subprocess.run(['powershell', '-NoProfile', '-Command',
                            "(New-Object -ComObject Shell.Application).Namespace(17).ParseName('%s').InvokeVerb('Eject')" % lecteur],
                           timeout=20, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except (OSError, subprocess.SubprocessError):
            pass
        self.bib, self.pub, self.annuler_pile = None, None, []
        self.remplir(garder=None)
        self.afficher(None)
        self.arreter_revue()
        self.etat_boutons()
        self.montrer('accueil')
        self.dire("Carte %s éjectée : remets-la dans le détecteur éteint, puis rallume-le." % lecteur)
        self.mb.showinfo("Carte SD", "Carte éjectée.\n\nRemets-la dans le détecteur éteint, puis rebranche-le :\n"
                         "au démarrage, il relit la base corrigée.")

    def restaurer(self):
        b = self.bib
        if not b:
            return
        liste = b.sauvegardes()
        if not liste:
            self.mb.showinfo("Restaurer", "Aucune sauvegarde dans %s" % DOSSIER_SAUV)
            return
        def nom(c):
            return os.path.basename(c) + ("  (ancienne, sur la carte)" if os.path.dirname(c) != DOSSIER_SAUV else "")
        texte = "\n".join("%d. %s" % (i + 1, nom(n)) for i, n in enumerate(liste[:15]))
        k = self.sd.askinteger("Restaurer une sauvegarde", "Sauvegardes (la plus récente en premier) :\n\n%s\n\nNuméro à remettre :" % texte,
                               minvalue=1, maxvalue=min(15, len(liste)), parent=self.r)
        if not k:
            return
        if not self.mb.askyesno("Restaurer", "Remettre la bibliothèque du %s ?\nLa bibliothèque actuelle sera remplacée." % os.path.basename(liste[k - 1])):
            return
        try:
            b.restaurer(liste[k - 1])
        except OSError as e:
            self.mb.showerror("Restaurer", str(e))
            return
        self.annuler_pile, self.pub = [], None
        self.remplir(garder=None)
        self.afficher(None)
        self.dire("Sauvegarde %s remise en place%s." % (os.path.basename(liste[k - 1]),
                  " : « Enregistrer et préparer l'envoi » pour l'envoyer à la carte" if self.mode_zip else " sur la carte"))
        if self.mode_zip:
            self.bib.modifie = True
            self.etat_boutons()

    def ouvrir_sauvegardes(self):
        if not self.bib:
            return
        d = self.bib.dossier_sauvegardes
        os.makedirs(d, exist_ok=True)
        if sys.platform.startswith('win'):
            os.startfile(d)
        else:
            subprocess.Popen(['xdg-open' if sys.platform.startswith('linux') else 'open', d])

    # ================================================================ v0.9.55 : nettoyage de la carte SD
    def choisir_sd(self, titre):
        """racine de la carte SD branchee sur le PC (celle deja ouverte en ancien mode, sinon on demande)"""
        defaut = None
        if self.bib and not self.mode_zip:
            defaut = racine_sd(self.bib.dossier)
        chemin = self.fd.askdirectory(title=titre, mustexist=True, **({'initialdir': defaut} if defaut else {}))
        if not chemin:
            return None
        r = racine_sd(chemin)
        if r and os.path.abspath(r).startswith(os.path.abspath(DOSSIER_PC)):
            self.mb.showwarning(titre, "Ce dossier est un dossier de travail du PC (DetecteurPub), pas une carte SD.")
            return None
        if not r:
            self.mb.showerror(titre, "Pas de dossier « pub » ici.\n\nChoisis la racine de la carte SD du détecteur (ex. E:\\).")
        return r

    def ouvrir_sd(self, choisir=False):
        """MODE 1 : la carte SD du detecteur, branchee sur le PC (trouvee toute seule si possible)"""
        if self.bib and self.bib.modifie and not self.mb.askyesno(
                "Modifications", "Abandonner les modifications non enregistrées ?"):
            return False
        cartes = [] if choisir else cartes_sd()
        if len(cartes) == 1:
            r = cartes[0]
        else:
            if not choisir:
                self.mb.showinfo("Carte SD", ("Plusieurs cartes du détecteur sont branchées : choisis la bonne."
                                              if cartes else "Je ne trouve pas la carte SD du détecteur.\n\n"
                                              "Est-elle bien dans le lecteur ? Choisis-la (ex. G:)."))
            r = self.choisir_sd("Choisis la carte SD du détecteur (ex. G:\\)")
            if not r:
                return False
        if self.bib:
            self.bib.modifie = False
        self.mettre_a_niveau(r, lambda: self.ouvrir(r))
        return True

    def mettre_a_niveau(self, racine, ensuite):
        """v0.9.56 : carte a l'ancien format (restes oranges du nettoyage) : proposer la mise a niveau.
        Oui : copie complete .zip sur le PC, puis effacement des seuls elements oranges. Puis ensuite()."""
        try:
            lignes, _, bib = analyser_sd(racine)
        except OSError as e:
            self.mb.showerror("Carte SD", str(e))
            return
        vieux = [l for l in lignes if l.cat == 'inutile' and l.coche]
        if not vieux or bib is None:
            ensuite()
            return
        liste = "\n".join("  • " + l.titre for l in vieux[:10]) + ("\n  • …" if len(vieux) > 10 else "")
        if not self.mb.askyesno("Carte SD à l'ancien format",
                                "Cette carte a des éléments de l'ancien format qui ne servent plus :\n\n%s\n\n"
                                "La remettre à niveau maintenant ?\n\n"
                                "D'abord, toute la carte est copiée sur le PC (DetecteurPub\\sauvegardes_carte_sd).\n"
                                "Tes pubs, leurs sons et les captations ne sont pas touchés." % liste):
            ensuite()
            return
        self.travail_long("Mise à niveau de la carte SD",
                          lambda progres: (copier_sd(racine, vieux, progres), appliquer_nettoyage(racine, vieux)),
                          lambda res, err: self.fin_mise_a_niveau(res, err, ensuite))

    def fin_mise_a_niveau(self, res, err, ensuite):
        if err:
            self.mb.showerror("Mise à niveau", "%s\n\nLa carte n'a pas été modifiée." % err)
        else:
            z, (libere, erreurs) = res
            msg = "Carte remise à niveau : %s libérés.\nCopie complète d'avant : %s" % (fmt_taille(libere), os.path.basename(z))
            if erreurs:
                msg += "\n\nNon traités :\n" + "\n".join(erreurs[:8])
            self.mb.showinfo("Mise à niveau", msg)
        ensuite()

    def travail_long(self, titre, travail, fin):
        """fait travail(progres) dans un fil a part, avec une barre ; puis fin(resultat, erreur)"""
        import threading
        tk, ttk = self.tk, self.ttk
        w = tk.Toplevel(self.r)
        w.title(titre)
        w.transient(self.r)
        w.resizable(False, False)
        w.protocol('WM_DELETE_WINDOW', lambda: None)
        f = ttk.Frame(w, padding=22)
        f.pack()
        lbl = ttk.Label(f, text="Copie complète de la carte sur le PC…", font=('Segoe UI', 11))
        lbl.pack(anchor='w')
        barre = ttk.Progressbar(f, length=420, mode='determinate', maximum=1.0)
        barre.pack(pady=(10, 4))
        ttk.Label(f, text="Ne retire pas la carte SD.", style='Sous.TLabel').pack(anchor='w')
        etat = {'p': 0.0, 'fin': False, 'res': None, 'err': None}

        def fil():
            try:
                etat['res'] = travail(lambda p: etat.__setitem__('p', p))
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                etat['err'] = str(e)
            etat['fin'] = True
        threading.Thread(target=fil, daemon=True).start()

        def suivre():
            if not etat['fin']:
                barre['value'] = etat['p']
                lbl.configure(text="Copie complète de la carte sur le PC… %d %%" % int(etat['p'] * 100) if etat['p'] < 1 else "Nettoyage…")
                w.after(150, suivre)
                return
            w.destroy()
            fin(etat['res'], etat['err'])
        suivre()

    def nettoyer_sd(self):
        if self.bib and self.bib.modifie and not self.mode_zip:
            if not self.mb.askyesno("Nettoyer", "La carte SD ouverte a des modifications non enregistrées.\n\nLes enregistrer d'abord ?"):
                return
            self.enregistrer()
            if self.bib and self.bib.modifie:
                return
        cartes = cartes_sd()
        r = cartes[0] if len(cartes) == 1 else self.choisir_sd("Nettoyer : choisis la carte SD du détecteur (ex. G:\\)")
        if r:
            FenetreNettoyage(self, r)

    def remettre_sd(self):
        r = self.choisir_sd("Remettre une copie : choisis la carte SD du détecteur (ex. E:\\)")
        if not r:
            return
        z = self.fd.askopenfilename(title="Copie complète à remettre", initialdir=DOSSIER_COPIES_SD if os.path.isdir(DOSSIER_COPIES_SD) else DOSSIER_PC,
                                    filetypes=[("Copie de carte SD", "sd_complete_*.zip"), ("ZIP", "*.zip")])
        if not z:
            return
        if not self.mb.askyesno("Remettre une copie complète",
                                "Le dossier « pub » de la carte %s va être remplacé entièrement par la copie\n%s\n\n"
                                "Tout ce qui a changé sur la carte depuis cette copie sera perdu. Continuer ?" % (r, os.path.basename(z))):
            return
        try:
            self.r.configure(cursor='watch')
            self.r.update_idletasks()
            remettre_copie_sd(z, r)
        except (OSError, ValueError, zipfile.BadZipFile) as e:
            self.mb.showerror("Remettre", str(e))
            return
        finally:
            self.r.configure(cursor='')
        if self.bib and not self.mode_zip and racine_sd(self.bib.dossier) == r:
            self.bib.modifie = False
            self.ouvrir(r)
        self.dire("Copie %s remise sur la carte %s." % (os.path.basename(z), r))
        self.mb.showinfo("Remettre", "La carte est revenue à l'état de la copie.\n\nÉjecte-la, puis remets-la dans le détecteur éteint.")

    def quitter(self):
        if self.bib and self.bib.modifie:
            r = self.mb.askyesnocancel("Quitter", "Enregistrer les modifications avant de quitter ?")
            if r is None:
                return
            if r:
                self.enregistrer()
                if self.bib and self.bib.modifie:
                    return
        self.lecteur.arreter()
        self.r.destroy()

    def lancer(self):
        self.r.mainloop()


if __name__ == '__main__':
    Application(sys.argv[1] if len(sys.argv) > 1 else None).lancer()
