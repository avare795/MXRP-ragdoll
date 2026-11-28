#!/usr/bin/env python3
"""v1.8.202 : signe les fichiers d'une release (mises a jour automatiques).
Usage : python3 tools/sign_release.py FICHIER [FICHIER...]
Ecrit FICHIER.sig (64 octets : r||s, ECDSA P-256 / SHA-256) a cote de chaque fichier.
La cle secrete est keys/maj_signature_privee.pem (depot prive seulement, jamais dans le depot public).
Les .sig sont a ajouter comme assets de la release, avec le meme nom que le fichier + .sig."""
import os, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

here = os.path.dirname(os.path.abspath(__file__))
key = serialization.load_pem_private_key(open(os.path.join(here, '..', 'keys', 'maj_signature_privee.pem'), 'rb').read(), None)
pub = key.public_key()
for f in sys.argv[1:]:
    data = open(f, 'rb').read()
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    sig = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
    pub.verify(encode_dss_signature(int.from_bytes(sig[:32], 'big'), int.from_bytes(sig[32:], 'big')), data,
               ec.ECDSA(hashes.SHA256()))  # (relu tel qu'il sera verifie par le plugin)
    open(f + '.sig', 'wb').write(sig)
    print('signe :', f + '.sig')
