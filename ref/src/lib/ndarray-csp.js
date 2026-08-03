/**
 * [Input] Flat JavaScript arrays, typed arrays, Buffers, or generic get/set stores plus ndarray shape metadata.
 * [Output] CSP-safe ndarray-compatible views without eval or Function-constructor code generation.
 * [Pos] Compatibility lib used by the production background-removal bundle transform.
 * [Sync] If this file changes, update background-removal-csp.test.js and ref/src/.folder.md.
 */

const TYPED_ARRAY_DTYPES = new Map([
  ["[object Float64Array]", "float64"],
  ["[object Float32Array]", "float32"],
  ["[object Int8Array]", "int8"],
  ["[object Int16Array]", "int16"],
  ["[object Int32Array]", "int32"],
  ["[object Uint8Array]", "uint8"],
  ["[object Uint16Array]", "uint16"],
  ["[object Uint32Array]", "uint32"],
  ["[object Uint8ClampedArray]", "uint8_clamped"],
  ["[object BigInt64Array]", "bigint64"],
  ["[object BigUint64Array]", "biguint64"],
]);

function isBuffer(data) {
  return Boolean(
    data
      && data.constructor
      && typeof data.constructor.isBuffer === "function"
      && data.constructor.isBuffer(data),
  );
}

function arrayDType(data) {
  if (isBuffer(data)) return "buffer";
  const typedArrayDType = TYPED_ARRAY_DTYPES.get(Object.prototype.toString.call(data));
  if (typedArrayDType) return typedArrayDType;
  if (Array.isArray(data)) return "array";
  return "generic";
}

function defaultStride(shape) {
  const stride = new Array(shape.length);
  for (let index = shape.length - 1, size = 1; index >= 0; index -= 1) {
    stride[index] = size;
    size *= shape[index];
  }
  return stride;
}

function defaultOffset(shape, stride) {
  let offset = 0;
  for (let index = 0; index < shape.length; index += 1) {
    if (stride[index] < 0) offset -= (shape[index] - 1) * stride[index];
  }
  return offset;
}

class NdArrayView {
  constructor(data, shape, stride, offset, dtype, nil = false) {
    this.data = data;
    this.shape = shape;
    this.stride = stride;
    this.offset = offset | 0;
    this.dtype = dtype;
    this.dimension = nil ? -1 : shape.length;
    this._generic = dtype === "generic";
  }

  get size() {
    if (this.dimension < 0) return 0;
    if (this.dimension === 0) return 1;
    return this.shape.reduce((size, value) => size * value, 1);
  }

  get order() {
    return this.stride
      .map((value, index) => [Math.abs(value), index])
      .sort((left, right) => left[0] - right[0])
      .map((entry) => entry[1]);
  }

  index(...indices) {
    if (this.dimension < 0) return -1;
    let result = this.offset;
    for (let axis = 0; axis < this.dimension; axis += 1) {
      result += this.stride[axis] * indices[axis];
    }
    return result;
  }

  get(...indices) {
    if (this.dimension < 0) return undefined;
    const index = this.index(...indices);
    return this._generic ? this.data.get(index) : this.data[index];
  }

  set(...args) {
    if (this.dimension < 0) return undefined;
    const value = args[this.dimension];
    const index = this.index(...args);
    if (this._generic) return this.data.set(index, value);
    this.data[index] = value;
    return value;
  }

  hi(...limits) {
    if (this.dimension < 0) return createNilView(this.data, this.dtype);
    if (this.dimension === 0) return createView(this.data, [], [], this.offset, this.dtype);
    const shape = this.shape.map((value, axis) => (
      typeof limits[axis] === "number" && limits[axis] >= 0
        ? limits[axis] | 0
        : value
    ));
    return createView(this.data, shape, this.stride.slice(), this.offset, this.dtype);
  }

  lo(...starts) {
    if (this.dimension < 0) return createNilView(this.data, this.dtype);
    if (this.dimension === 0) return createView(this.data, [], [], this.offset, this.dtype);
    const shape = this.shape.slice();
    let offset = this.offset;
    for (let axis = 0; axis < this.dimension; axis += 1) {
      if (typeof starts[axis] === "number" && starts[axis] >= 0) {
        const start = starts[axis] | 0;
        offset += this.stride[axis] * start;
        shape[axis] -= start;
      }
    }
    return createView(this.data, shape, this.stride.slice(), offset, this.dtype);
  }

  step(...steps) {
    if (this.dimension < 0) return createNilView(this.data, this.dtype);
    if (this.dimension === 0) return createView(this.data, [], [], this.offset, this.dtype);
    const shape = this.shape.slice();
    const stride = this.stride.slice();
    let offset = this.offset;
    for (let axis = 0; axis < this.dimension; axis += 1) {
      if (typeof steps[axis] !== "number") continue;
      const step = steps[axis] | 0;
      if (step < 0) {
        offset += stride[axis] * (shape[axis] - 1);
        shape[axis] = Math.ceil(-shape[axis] / step);
      } else {
        shape[axis] = Math.ceil(shape[axis] / step);
      }
      stride[axis] *= step;
    }
    return createView(this.data, shape, stride, offset, this.dtype);
  }

  transpose(...axes) {
    if (this.dimension < 0) return createNilView(this.data, this.dtype);
    if (this.dimension === 0) return createView(this.data, [], [], this.offset, this.dtype);
    const normalizedAxes = this.shape.map((_, index) => (
      axes[index] === undefined ? index : axes[index] | 0
    ));
    return createView(
      this.data,
      normalizedAxes.map((axis) => this.shape[axis]),
      normalizedAxes.map((axis) => this.stride[axis]),
      this.offset,
      this.dtype,
    );
  }

  pick(...indices) {
    if (this.dimension < 0) return null;
    if (this.dimension === 0) return createNilView(this.data, this.dtype);
    const shape = [];
    const stride = [];
    let offset = this.offset;
    for (let axis = 0; axis < this.dimension; axis += 1) {
      if (typeof indices[axis] === "number" && indices[axis] >= 0) {
        offset = (offset + this.stride[axis] * indices[axis]) | 0;
      } else {
        shape.push(this.shape[axis]);
        stride.push(this.stride[axis]);
      }
    }
    return createView(this.data, shape, stride, offset, this.dtype);
  }

  valueOf() {
    return this.dimension === 0 ? this.get() : this;
  }
}

class NdArrayView3 extends NdArrayView {
  index(i0, i1, i2) {
    return this.offset + this.stride[0] * i0 + this.stride[1] * i1 + this.stride[2] * i2;
  }

  get(i0, i1, i2) {
    const index = this.index(i0, i1, i2);
    return this._generic ? this.data.get(index) : this.data[index];
  }

  set(i0, i1, i2, value) {
    const index = this.index(i0, i1, i2);
    if (this._generic) return this.data.set(index, value);
    this.data[index] = value;
    return value;
  }
}

function createNilView(data, dtype) {
  return new NdArrayView(data, [], [], 0, dtype, true);
}

function createView(data, shape, stride, offset, dtype) {
  const View = shape.length === 3 ? NdArrayView3 : NdArrayView;
  return new View(data, shape, stride, offset, dtype);
}

export default function ndarray(data, shape, stride, offset) {
  if (data === undefined) return createNilView([], "array");
  const normalizedData = typeof data === "number" ? [data] : data;
  const normalizedShape = shape === undefined ? [normalizedData.length] : shape.slice();
  const normalizedStride = stride === undefined ? defaultStride(normalizedShape) : stride.slice();
  const normalizedOffset = offset === undefined
    ? defaultOffset(normalizedShape, normalizedStride)
    : offset;
  return createView(
    normalizedData,
    normalizedShape,
    normalizedStride,
    normalizedOffset,
    arrayDType(normalizedData),
  );
}
