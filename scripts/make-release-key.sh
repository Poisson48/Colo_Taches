#!/usr/bin/env bash
# Génère LA clé de publication de Colo Tâches — une fois pour toutes.
#
# Pourquoi : Android identifie une app par sa signature. Deux APK signés par des clés
# différentes sont deux apps étrangères l'une à l'autre : la mise à jour par-dessus
# est refusée, et il faut désinstaller (donc perdre les listes locales). La clé doit
# rester la même pour toutes les versions à venir.
#
# À faire une seule fois :
#   bash scripts/make-release-key.sh
# puis suivre les instructions affichées (3 secrets à coller dans GitHub).
#
# ⚠️  SAUVEGARDEZ le fichier .jks produit et son mot de passe, hors du dépôt.
#     Clé perdue = plus aucune mise à jour possible pour les utilisateurs installés.
set -euo pipefail

OUT="${OUT:-$HOME/colotaches-release.jks}"
ALIAS="${ALIAS:-colotaches}"

if [ -f "$OUT" ]; then
  echo "Un keystore existe déjà : $OUT" >&2
  echo "Ne le régénérez pas — ce serait une nouvelle identité d'app." >&2
  echo "Pour réafficher le secret : base64 -w0 \"$OUT\"" >&2
  exit 1
fi

# Mot de passe aléatoire : il n'a pas à être mémorisé, il vivra dans les secrets
# GitHub et dans votre sauvegarde du keystore.
# Entrée bornée (head -c sur /dev/urandom) plutôt qu'un `tr </dev/urandom | head` :
# ce dernier tue `tr` par SIGPIPE, que `set -o pipefail` transforme en échec du script.
STOREPASS="$(head -c 48 /dev/urandom | base64 | tr -d '/+=' | cut -c1-32)"

keytool -genkeypair \
  -keystore "$OUT" -alias "$ALIAS" \
  -storepass "$STOREPASS" -keypass "$STOREPASS" \
  -keyalg RSA -keysize 4096 -validity 10000 \
  -dname "CN=Colo Tâches, O=ColoTaches, C=FR" >/dev/null

chmod 600 "$OUT"

REPO="$(git -C "$(dirname "$0")/.." remote get-url origin 2>/dev/null || echo '<votre/dépôt>')"

cat <<EOF

Keystore créé : $OUT   (alias : $ALIAS)

1. Sauvegardez ce fichier et ce mot de passe ailleurs que dans le dépôt :

     $STOREPASS

2. Déclarez les trois secrets dans GitHub (gh CLI) :

     gh secret set ANDROID_KEYSTORE_B64 --body "\$(base64 -w0 "$OUT")"
     gh secret set ANDROID_KEY_ALIAS    --body "$ALIAS"
     gh secret set ANDROID_KEYSTORE_PASS --body "$STOREPASS"

   (ou à la main : $REPO → Settings → Secrets and variables → Actions)

3. La prochaine release sera signée avec cette clé, et toutes les suivantes aussi.

⚠️  La version déjà installée sur votre téléphone porte une signature aléatoire
    (générée par le CI à chaque build) : une dernière désinstallation est nécessaire
    avant d'installer la première release signée avec cette clé. Ensuite, les mises
    à jour s'installeront par-dessus, sans perte de données.
EOF
