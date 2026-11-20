#!/usr/bin/env bash
# Does the production server sign licence tokens with the key shipped builds trust?
#
# Read-only. Derives the public half server-side and prints MATCH / MISMATCH.
# The private key is never printed, copied or sent anywhere.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH_USER="${SSH_USER:-u941499432}"
SSH_HOST="${SSH_HOST:-145.79.30.42}"
SSH_PORT="${SSH_PORT:-65002}"
APP_DIR="${APP_DIR:-domains/nexadownloadmanager.com/nexa-api}"

EXPECTED="$(tr -d '[:space:]' < "$REPO/packaging/license-public-key.txt" | tr 'A-F' 'a-f')"
[ -n "$EXPECTED" ] || { echo "FAIL: packaging/license-public-key.txt is empty"; exit 2; }
echo "shipped public key (compiled into the app): $EXPECTED"

REMOTE='
cd '"$APP_DIR"' || exit 9
NODE=$(ls -d /opt/alt/alt-nodejs*/root/usr/bin/node 2>/dev/null | tail -1)
[ -n "$NODE" ] || NODE=$(command -v node)
[ -n "$NODE" ] || { echo "ERR no node"; exit 9; }
"$NODE" -e '"'"'
const fs=require("fs"),crypto=require("crypto");
let env=""; try{env=fs.readFileSync(".env","utf8");}catch(e){console.log("ERR no .env");process.exit(9);}
const m=env.match(/^LICENSE_JWT_PRIVATE_KEY=(.*)$/m);
if(!m){console.log("ERR LICENSE_JWT_PRIVATE_KEY not set");process.exit(9);}
let v=m[1].trim().replace(/^["\x27]|["\x27]$/g,"").replace(/\\n/g,"\n");
const P=Buffer.from("302e020100300506032b657004220420","hex");
let key;
try{
  if(v.includes("BEGIN")) key=crypto.createPrivateKey(v);
  else {
    const raw=/^[0-9a-fA-F]{64}$/.test(v)?Buffer.from(v,"hex"):Buffer.from(v,"base64");
    if(raw.length!==32) throw new Error("seed is "+raw.length+" bytes, expected 32");
    key=crypto.createPrivateKey({key:Buffer.concat([P,raw]),format:"der",type:"pkcs8"});
  }
}catch(e){console.log("ERR unreadable key: "+e.message);process.exit(9);}
if(key.asymmetricKeyType!=="ed25519"){console.log("ERR key is "+key.asymmetricKeyType+", not ed25519");process.exit(9);}
const hex=crypto.createPublicKey(key).export({type:"spki",format:"der"}).subarray(-32).toString("hex");
console.log("HEX "+hex);
'"'"'
'

if [ -n "${SSHPASS:-}" ] && command -v sshpass >/dev/null 2>&1; then
  SSH_CMD=(sshpass -e ssh -o StrictHostKeyChecking=no)
else
  SSH_CMD=(ssh)
fi
OUT="$("${SSH_CMD[@]}" -p "$SSH_PORT" -o ConnectTimeout=25 "${SSH_USER}@${SSH_HOST}" "$REMOTE" 2>&1 | tail -1)"

case "$OUT" in
  HEX\ *)
    ACTUAL="${OUT#HEX }"
    echo "server signing key (public half):           $ACTUAL"
    echo
    if [ "$ACTUAL" = "$EXPECTED" ]; then
      echo "MATCH — deploying will mint tokens the shipped app accepts."
      exit 0
    fi
    echo "MISMATCH — do NOT deploy the backend yet."
    exit 1 ;;
  *) echo "could not determine: $OUT"; exit 2 ;;
esac
