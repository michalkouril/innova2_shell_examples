# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# train.py -- hls4ml's jet-tagging benchmark model (16 -> 64 -> 32 -> 32 -> 5, ReLU), trained on the OpenML dataset
# hls4ml_lhc_jets_hlf (OpenML 42468); writes model.h5, the standardisation (scaler.npz) and the held-out test set (test.npz).
import os, numpy as np
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import hashlib, pandas as pd
from sklearn.preprocessing import LabelEncoder, StandardScaler
from sklearn.model_selection import train_test_split
import tensorflow as tf
tf.keras.utils.set_random_seed(1)
ARFF = 'dl/hls4ml_lhc_jets_hlf.arff'   # https://openml.org/data/v1/download/21854360 (OpenML 42468), md5 below
assert hashlib.md5(open(ARFF, 'rb').read()).hexdigest() == 'b0cade1d843feb6199c746918453c378', 'dataset md5'
hdr = []; skip = 0
for l in open(ARFF):
    skip += 1
    if l.lower().startswith('@attribute'): hdr.append(l.split()[1].strip("'"))
    if l.lower().startswith('@data'): break
df = pd.read_csv(ARFF, skiprows=skip, header=None, names=hdr)
X, y = df[hdr[:16]].to_numpy(np.float32), df[hdr[16]].astype(str)
le = LabelEncoder(); yi = le.fit_transform(y); Y = tf.keras.utils.to_categorical(yi, 5)
Xtr, Xte, Ytr, Yte = train_test_split(X, Y, test_size=0.2, random_state=42)
sc = StandardScaler().fit(Xtr); Xtr = sc.transform(Xtr).astype(np.float32); Xte = sc.transform(Xte).astype(np.float32)
L = tf.keras.layers
m = tf.keras.Sequential([L.Input((16,)), L.Dense(64, activation='relu', name='fc1'), L.Dense(32, activation='relu', name='fc2'),
                         L.Dense(32, activation='relu', name='fc3'), L.Dense(5, activation='linear', name='out')])
m.compile(optimizer=tf.keras.optimizers.Adam(1e-3), loss=tf.keras.losses.CategoricalCrossentropy(from_logits=True), metrics=['accuracy'])
m.fit(Xtr, Ytr, batch_size=1024, epochs=12, validation_split=0.1, verbose=2)
print('KERAS-TEST-ACC %.4f' % m.evaluate(Xte, Yte, verbose=0)[1])
m.save('model.h5'); np.savez('scaler.npz', mean=sc.mean_, scale=sc.scale_, classes=le.classes_)
np.savez('test.npz', X=Xte, Y=Yte)
