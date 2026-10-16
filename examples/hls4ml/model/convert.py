# SPDX-FileCopyrightText: 2026 the innova2 contributors
# SPDX-License-Identifier: Apache-2.0
# convert.py -- model.h5 -> hls4ml project (Vitis backend, io_parallel, latency strategy, ReuseFactor 16, ap_fixed<16,6>)
# for the FLD-lite inline kernel; compares hls4ml's bit-accurate C emulation with Keras on the test set and writes the
# packet test vectors (inputs as raw ap_fixed<16,6> int16, expected raw outputs) to vectors.npz.
import os, numpy as np
os.environ.setdefault('TF_CPP_MIN_LOG_LEVEL', '2')
import tensorflow as tf, hls4ml
m = tf.keras.models.load_model('model.h5', compile=False)
cfg = hls4ml.utils.config_from_keras_model(m, granularity='model', backend='Vitis')
cfg['Model']['Precision'] = 'ap_fixed<16,6>'; cfg['Model']['ReuseFactor'] = int(os.environ.get('H4ML_RF', 16)); cfg['Model']['Strategy'] = os.environ.get('H4ML_STRATEGY', 'Latency')
h = hls4ml.converters.convert_from_keras_model(m, hls_config=cfg, backend='Vitis', output_dir=os.environ.get('H4ML_PRJ', 'hls4ml_prj'),
        project_name='jetnet', part='xcku15p-ffve1517-2-i', clock_period=3.333, io_type='io_parallel')
h.compile()
t = np.load('test.npz'); X = t['X']; Y = t['Y']
q = np.clip(np.round(X * 1024), -32768, 32767).astype(np.int16)        # what goes into the packet (ap_fixed<16,6> raw)
Xq = q.astype(np.float32) / 1024
yk = m.predict(X, batch_size=4096, verbose=0)
yh = h.predict(np.ascontiguousarray(Xq))
print('ACC keras %.4f  hls4ml(fixed, quantised inputs) %.4f  argmax agreement %.4f' % (
    (yk.argmax(1) == Y.argmax(1)).mean(), (yh.argmax(1) == Y.argmax(1)).mean(), (yk.argmax(1) == yh.argmax(1)).mean()))
out = np.round(yh * 1024).astype(np.int64)
assert np.all(np.abs(out) < 32768) and np.allclose(out / 1024, yh), 'outputs are not ap_fixed<16,6> values'
np.savez(os.environ.get('H4ML_VEC', 'vectors.npz'), in_raw=q, out_raw=out.astype(np.int16), label=Y.argmax(1))
print('VECTORS', q.shape)
