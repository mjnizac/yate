#include <engine/terrain/raster.hpp>

#include <engine/memory/general.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <string>
#include <vector>

namespace engine::terrain::raster {

namespace {

constexpr f64_t kInfinity = 1e20;

/// The lower envelope of the parabolas `f[p] + (q - p)^2` along one line, written to `out`. `v` and `zb`
/// are the envelope's lists, kept by the caller from line to line.
void Envelope(const f64_t* f, u32_t count, f64_t* out, i32_t* v, f64_t* zb) {
    v[0]  = 0;
    zb[0] = -kInfinity;
    zb[1] = kInfinity;
    i32_t k = 0;
    for (i32_t q = 1; q < static_cast<i32_t>(count); ++q) {
        const f64_t fq = f[q] + static_cast<f64_t>(q) * q;
        i32_t       p  = v[k];
        f64_t       s  = (fq - (f[p] + static_cast<f64_t>(p) * p)) / (2.0 * (q - p));
        while (s <= zb[k]) {
            --k;
            p = v[k];
            s = (fq - (f[p] + static_cast<f64_t>(p) * p)) / (2.0 * (q - p));
        }
        ++k;
        v[k]      = q;
        zb[k]     = s;
        zb[k + 1] = kInfinity;
    }
    k = 0;
    for (i32_t q = 0; q < static_cast<i32_t>(count); ++q) {
        while (zb[k + 1] < q) {
            ++k;
        }
        const i32_t p = v[k];
        out[q]        = static_cast<f64_t>(q - p) * (q - p) + f[p];
    }
}

} // namespace

void Distance(std::span<const u8_t> source, u32_t n, std::span<f64_t> out) {
    const usize_t count = static_cast<usize_t>(n) * n;
    for (usize_t i = 0; i < count; ++i) {
        out[i] = source[i] != 0 ? 0.0 : kInfinity;
    }
    // Down the columns: the squared distance to the nearest source in the column, a scan each way.
    for (u32_t x = 0; x < n; ++x) {
        f64_t run = kInfinity;
        for (u32_t z = 0; z < n; ++z) {
            f64_t& d = out[static_cast<usize_t>(z) * n + x];
            if (d == 0.0) {
                run = 0.0;
            } else if (run < kInfinity) {
                run += 1.0;
            }
            d = run;
        }
        run = kInfinity;
        for (u32_t z = n; z-- > 0;) {
            f64_t& d = out[static_cast<usize_t>(z) * n + x];
            if (d == 0.0) {
                run = 0.0;
            } else if (run < kInfinity) {
                run += 1.0;
            }
            d = std::min(d, run);
        }
        for (u32_t z = 0; z < n; ++z) {
            f64_t& d = out[static_cast<usize_t>(z) * n + x];
            if (d < kInfinity) {
                d *= d;
            }
        }
    }
    // Along the rows, the envelope.
    std::pmr::vector<f64_t> line(n, 0.0, &memory::General().Resource());
    std::pmr::vector<f64_t> zb(static_cast<usize_t>(n) + 1, 0.0, &memory::General().Resource());
    std::pmr::vector<i32_t> v(n, 0, &memory::General().Resource());
    for (u32_t z = 0; z < n; ++z) {
        f64_t* row = out.data() + static_cast<usize_t>(z) * n;
        std::copy(row, row + n, line.begin());
        Envelope(line.data(), n, row, v.data(), zb.data());
        for (u32_t x = 0; x < n; ++x) {
            row[x] = std::sqrt(row[x]);
        }
    }
}

void Square(std::span<u8_t> mask, u32_t n, u32_t radius, b8_t all) {
    std::pmr::vector<u8_t> buffer(mask.size(), 0, &memory::General().Resource());
    const auto pass = [&](const u8_t* input, u8_t* output, usize_t stride, usize_t step) {
        for (u32_t line = 0; line < n; ++line) {
            const usize_t base   = line * stride;
            i32_t         count  = 0;
            i32_t         inside = 0;
            for (u32_t q = 0; q <= std::min(radius, n - 1); ++q) {
                ++count;
                inside += input[base + q * step] != 0 ? 1 : 0;
            }
            for (u32_t q = 0; q < n; ++q) {
                output[base + q * step] = (all ? inside == count : inside > 0) ? 1 : 0;
                const u32_t add = q + radius + 1;
                if (add < n) {
                    ++count;
                    inside += input[base + add * step] != 0 ? 1 : 0;
                }
                if (q >= radius) {
                    --count;
                    inside -= input[base + (q - radius) * step] != 0 ? 1 : 0;
                }
            }
        }
    };
    pass(mask.data(), buffer.data(), n, 1);
    pass(buffer.data(), mask.data(), 1, n);
}

void Box(std::span<f64_t> values, u32_t n, u32_t radius) {
    if (radius == 0) {
        return;
    }
    std::pmr::vector<f64_t> buffer(values.size(), 0.0, &memory::General().Resource());
    const auto pass = [&](const f64_t* input, f64_t* output, usize_t stride, usize_t step) {
        for (u32_t line = 0; line < n; ++line) {
            const usize_t base  = line * stride;
            f64_t         sum   = 0.0;
            i32_t         count = 0;
            for (u32_t q = 0; q <= std::min(radius, n - 1); ++q) {
                sum += input[base + q * step];
                ++count;
            }
            for (u32_t q = 0; q < n; ++q) {
                output[base + q * step] = sum / count;
                const u32_t add = q + radius + 1;
                if (add < n) {
                    sum += input[base + add * step];
                    ++count;
                }
                if (q >= radius) {
                    sum -= input[base + (q - radius) * step];
                    --count;
                }
            }
        }
    };
    for (i32_t round = 0; round < 3; ++round) {
        pass(values.data(), buffer.data(), n, 1);
        pass(buffer.data(), values.data(), 1, n);
    }
}

void Project(std::span<const f64_t> sdf, u32_t n, f64_t cell, u32_t reach, std::span<const f64_t> atCoast,
             std::span<f64_t> out) {
    const i64_t last = static_cast<i64_t>(n) - 1;
    const auto  at   = [&](i64_t x, i64_t z) {
        return sdf[static_cast<usize_t>(std::clamp<i64_t>(z, 0, last) * n + std::clamp<i64_t>(x, 0, last))];
    };
    const f64_t far = static_cast<f64_t>(reach) * cell;
    for (i64_t z = 0; z <= last; ++z) {
        for (i64_t x = 0; x <= last; ++x) {
            const usize_t k    = static_cast<usize_t>(z * n + x);
            const f64_t   d    = sdf[k];
            i64_t         px   = x;
            i64_t         pz   = z;
            f64_t         half = 1.0e9;
            if (d >= far) {
                half = 0.0;
            } else if (d > 0.0) {
                f64_t       gx = at(x + 1, z) - at(x - 1, z);
                f64_t       gz = at(x, z + 1) - at(x, z - 1);
                const f64_t l  = std::sqrt(gx * gx + gz * gz);
                if (l > 0.0) {
                    gx /= l;
                    gz /= l;
                    px = std::clamp<i64_t>(static_cast<i64_t>(std::floor(x - gx * d / cell + 0.5)), 0, last);
                    pz = std::clamp<i64_t>(static_cast<i64_t>(std::floor(z - gz * d / cell + 0.5)), 0, last);
                    f64_t top = d;
                    for (u32_t s = 1; s <= reach; ++s) {
                        const f64_t v = at(static_cast<i64_t>(std::floor(x + gx * s + 0.5)),
                                           static_cast<i64_t>(std::floor(z + gz * s + 0.5)));
                        if (v < top - 0.25 * cell) {
                            half = top;
                            break;
                        }
                        top = std::max(top, v);
                    }
                }
            }
            out[k] = std::min(atCoast[static_cast<usize_t>(pz * n + px)], half / 3.0);
        }
    }
}

void Drain(std::span<const u8_t> land, std::span<const f64_t> height, u32_t n, std::span<f64_t> parent,
           std::span<f64_t> area) {
    const usize_t count = static_cast<usize_t>(n) * n;
    std::fill(parent.begin(), parent.end(), -1.0);
    std::fill(area.begin(), area.end(), 0.0);
    // A min-heap of (level, pixel): the lowest level first, the lower pixel of a tie.
    struct Entry {
        f64_t level;
        u32_t at;
    };
    const auto later = [](const Entry& a, const Entry& b) {
        return a.level > b.level || (a.level == b.level && a.at > b.at);
    };
    std::pmr::vector<Entry> heap(&memory::General().Resource());
    std::pmr::vector<f64_t> level(count, 0.0, &memory::General().Resource());
    std::pmr::vector<u32_t> order(&memory::General().Resource());
    heap.reserve(count);
    order.reserve(count);
    const auto inside = [n](u32_t x, u32_t z) { return x >= 1 && z >= 1 && x + 1 < n && z + 1 < n; };
    for (u32_t z = 1; z + 1 < n; ++z) {
        for (u32_t x = 1; x + 1 < n; ++x) {
            const usize_t k = static_cast<usize_t>(z) * n + x;
            if (land[k] && (!land[k - 1] || !land[k + 1] || !land[k - n] || !land[k + n])) {
                parent[k] = 0.0;
                level[k]  = height[k];
                heap.push_back({height[k], static_cast<u32_t>(k)});
                std::push_heap(heap.begin(), heap.end(), later);
            }
        }
    }
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), later);
        const u32_t k = heap.back().at;
        heap.pop_back();
        order.push_back(k);
        const u32_t x = k % n;
        const u32_t z = k / n;
        for (i32_t dz = -1; dz <= 1; ++dz) {
            for (i32_t dx = -1; dx <= 1; ++dx) {
                const u32_t xx = x + static_cast<u32_t>(dx);
                const u32_t zz = z + static_cast<u32_t>(dz);
                if ((dx == 0 && dz == 0) || !inside(xx, zz)) {
                    continue;
                }
                const usize_t q = static_cast<usize_t>(zz) * n + xx;
                if (land[q] && parent[q] < 0.0) {
                    parent[q] = static_cast<f64_t>(k) + 1.0;
                    level[q]  = std::max(height[q], level[k]);
                    heap.push_back({level[q], static_cast<u32_t>(q)});
                    std::push_heap(heap.begin(), heap.end(), later);
                }
            }
        }
    }
    // Down the paths, from the last pixel reached (the furthest upstream) to the outlets.
    for (usize_t i = order.size(); i-- > 0;) {
        const u32_t k = order[i];
        area[k] += 1.0;
        if (parent[k] > 0.0) {
            area[static_cast<usize_t>(parent[k]) - 1] += area[k];
        }
    }
}

void Triangles(const Lattice& g, u32_t n, std::span<f64_t> corners, std::span<f64_t> weights,
               std::span<f64_t> height, std::span<u8_t> found) {
    std::pmr::memory_resource* resource = &memory::General().Resource();
    // The triangles, bucketed by the lattice slots their bounding boxes cover: the points may have moved, and
    // a pixel looks only in its slot's bucket. Built as the Lua did, so a bucket lists the same triangles in
    // the same order and a pixel on a shared edge picks the same one.
    std::pmr::vector<std::array<u32_t, 3>> tris(resource);
    std::pmr::vector<std::pmr::vector<u32_t>> buckets(static_cast<usize_t>(g.nx) * g.nz,
                                                      std::pmr::vector<u32_t>(resource), resource);
    const auto slot = [&](f64_t v) { return static_cast<i64_t>(std::floor(v / g.spacing)); };
    const auto add = [&](u32_t a, u32_t b, u32_t c) {
        tris.push_back({a, b, c});
        const u32_t t  = static_cast<u32_t>(tris.size() - 1);
        const i64_t x0 = slot(std::min({g.px[a], g.px[b], g.px[c]}));
        const i64_t x1 = slot(std::max({g.px[a], g.px[b], g.px[c]}));
        const i64_t z0 = slot(std::min({g.pz[a], g.pz[b], g.pz[c]}));
        const i64_t z1 = slot(std::max({g.pz[a], g.pz[b], g.pz[c]}));
        for (i64_t bz = std::max<i64_t>(z0, 0); bz <= std::min<i64_t>(z1, g.nz - 1); ++bz) {
            for (i64_t bx = std::max<i64_t>(x0, 0); bx <= std::min<i64_t>(x1, g.nx - 1); ++bx) {
                buckets[static_cast<usize_t>(bz * g.nx + bx)].push_back(t);
            }
        }
    };
    for (u32_t bz = 0; bz + 1 < g.nz; ++bz) {
        for (u32_t bx = 0; bx + 1 < g.nx; ++bx) {
            const u32_t i = bz * g.nx + bx;
            const u32_t a = i, b = i + 1, c = i + g.nx, d = i + g.nx + 1;
            if (g.diag[i]) {
                add(a, b, d);
                add(a, d, c);
            } else {
                add(a, b, c);
                add(b, d, c);
            }
        }
    }
    const f64_t pixel = g.units / n;
    for (u32_t z = 0; z < n; ++z) {
        for (u32_t x = 0; x < n; ++x) {
            const usize_t k  = static_cast<usize_t>(z) * n + x;
            const f64_t   wx = (x + 0.5) * pixel, wz = (z + 0.5) * pixel;
            const i64_t   sx = std::min<i64_t>(slot(wx), g.nx - 1), sz = std::min<i64_t>(slot(wz), g.nz - 1);
            const std::pmr::vector<u32_t>& bucket = buckets[static_cast<usize_t>(sz * g.nx + sx)];
            b8_t hit = false;
            for (const u32_t t : bucket) {
                const auto [a, b, c] = tris[t];
                const f64_t x1 = g.px[a], z1 = g.pz[a], x2 = g.px[b], z2 = g.pz[b], x3 = g.px[c], z3 = g.pz[c];
                const f64_t det = (z2 - z3) * (x1 - x3) + (x3 - x2) * (z1 - z3);
                if (std::abs(det) < 1e-12) {
                    continue;
                }
                const f64_t l1 = ((z2 - z3) * (wx - x3) + (x3 - x2) * (wz - z3)) / det;
                const f64_t l2 = ((z3 - z1) * (wx - x3) + (x1 - x3) * (wz - z3)) / det;
                const f64_t l3 = 1.0 - l1 - l2;
                if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9) {
                    continue;
                }
                corners[3 * k] = a + 1.0, corners[3 * k + 1] = b + 1.0, corners[3 * k + 2] = c + 1.0;
                weights[3 * k] = l1, weights[3 * k + 1] = l2, weights[3 * k + 2] = l3;
                hit = true;
                break;
            }
            if (!hit) {
                // Off every triangle (the rim of the map): the nearest corner of the bucket's triangles.
                f64_t best = std::numeric_limits<f64_t>::infinity();
                u32_t at   = 0;
                for (const u32_t t : bucket) {
                    for (const u32_t i : tris[t]) {
                        const f64_t dd = (g.px[i] - wx) * (g.px[i] - wx) + (g.pz[i] - wz) * (g.pz[i] - wz);
                        if (dd < best) {
                            best = dd;
                            at   = i;
                        }
                    }
                }
                corners[3 * k] = corners[3 * k + 1] = corners[3 * k + 2] = at + 1.0;
                weights[3 * k] = 1.0, weights[3 * k + 1] = 0.0, weights[3 * k + 2] = 0.0;
            }
            found[k]  = hit ? 1 : 0;
            height[k] = weights[3 * k] * g.h[static_cast<usize_t>(corners[3 * k]) - 1]
                        + weights[3 * k + 1] * g.h[static_cast<usize_t>(corners[3 * k + 1]) - 1]
                        + weights[3 * k + 2] * g.h[static_cast<usize_t>(corners[3 * k + 2]) - 1];
        }
    }
}

void Label(std::span<const u8_t> land, u32_t n, std::span<f64_t> region, std::pmr::vector<Body>& masses,
           std::pmr::vector<Body>& waters) {
    std::pmr::memory_resource* resource = &memory::General().Resource();
    const usize_t              count    = static_cast<usize_t>(n) * n;
    // Each pixel's body, by its index into `bodies` plus one; 0 not met yet.
    std::pmr::vector<u32_t> label(count, 0, resource);
    std::pmr::vector<Body>  bodies(resource);
    std::pmr::vector<u8_t>  wet(resource);
    std::pmr::vector<u32_t> stack(resource);
    const auto around = [n](usize_t q, auto&& each) {
        const usize_t x = q % n, z = q / n;
        if (x > 0) {
            each(q - 1);
        }
        if (x + 1 < n) {
            each(q + 1);
        }
        if (z > 0) {
            each(q - n);
        }
        if (z + 1 < n) {
            each(q + n);
        }
    };
    for (usize_t start = 0; start < count; ++start) {
        if (label[start] != 0) {
            continue;
        }
        const u8_t water = land[start] ? 0 : 1;
        bodies.push_back({});
        wet.push_back(water);
        const u32_t id = static_cast<u32_t>(bodies.size());
        Body&       body = bodies.back();
        label[start]     = id;
        stack.assign(1, static_cast<u32_t>(start));
        while (!stack.empty()) {
            const usize_t q = stack.back();
            stack.pop_back();
            const usize_t x = q % n, z = q / n;
            body.cells += 1.0;
            body.x += static_cast<f64_t>(x);
            body.z += static_cast<f64_t>(z);
            body.edge = body.edge || x == 0 || z == 0 || x == n - 1 || z == n - 1;
            around(q, [&](usize_t m) {
                if (label[m] == 0 && (land[m] ? 0 : 1) == water) {
                    label[m] = id;
                    stack.push_back(static_cast<u32_t>(m));
                }
            });
        }
    }
    // Ranks: the masses largest first, ties in the order met.
    const auto larger = [&](u32_t a, u32_t b) { return bodies[a].cells > bodies[b].cells; };
    std::pmr::vector<u32_t> order(resource);
    for (u32_t i = 0; i < bodies.size(); ++i) {
        if (!wet[i]) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), larger);
    std::pmr::vector<f64_t> rank(bodies.size(), 0.0, resource);
    masses.clear();
    for (u32_t r = 0; r < order.size(); ++r) {
        rank[order[r]] = r + 1.0;
        masses.push_back(bodies[order[r]]);
    }
    order.clear();
    for (u32_t i = 0; i < bodies.size(); ++i) {
        if (wet[i]) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), larger);
    waters.clear();
    for (const u32_t i : order) {
        waters.push_back(bodies[i]);
    }
    // The sea takes the id of the nearest land, grown outwards a ring at a time.
    std::pmr::vector<u32_t> front(resource), next(resource);
    for (usize_t i = 0; i < count; ++i) {
        region[i] = land[i] ? rank[label[i] - 1] : 0.0;
        if (region[i] > 0.0) {
            front.push_back(static_cast<u32_t>(i));
        }
    }
    while (!front.empty()) {
        next.clear();
        for (const u32_t q : front) {
            around(q, [&](usize_t m) {
                if (region[m] == 0.0) {
                    region[m] = region[q];
                    next.push_back(static_cast<u32_t>(m));
                }
            });
        }
        front.swap(next);
    }
}

void Soften(std::span<const f64_t> values, u32_t n, std::span<f64_t> out) {
    for (u32_t z = 0; z < n; ++z) {
        for (u32_t x = 0; x < n; ++x) {
            f64_t sum = 0.0, w = 0.0;
            for (i32_t dz = -1; dz <= 1; ++dz) {
                for (i32_t dx = -1; dx <= 1; ++dx) {
                    const i64_t   xx = std::clamp<i64_t>(static_cast<i64_t>(x) + dx, 0, n - 1);
                    const i64_t   zz = std::clamp<i64_t>(static_cast<i64_t>(z) + dz, 0, n - 1);
                    const f64_t   k  = (dx == 0 && dz == 0) ? 4.0 : ((dx == 0 || dz == 0) ? 2.0 : 1.0);
                    sum += values[static_cast<usize_t>(zz) * n + static_cast<usize_t>(xx)] * k;
                    w += k;
                }
            }
            out[static_cast<usize_t>(z) * n + x] = sum / w;
        }
    }
}

void Resample(std::span<const f64_t> values, const Placement& from, const Placement& to, b8_t nearest,
              f64_t outside, std::span<f64_t> out) {
    const f64_t size = static_cast<f64_t>(from.n);
    for (u32_t z = 0; z < to.n; ++z) {
        for (u32_t x = 0; x < to.n; ++x) {
            // In `from`'s pixels, measured from its first pixel's centre.
            const f64_t u = (to.x0 + (x + 0.5) * to.cell - from.x0) / from.cell - 0.5;
            const f64_t v = (to.z0 + (z + 0.5) * to.cell - from.z0) / from.cell - 0.5;
            f64_t&      o = out[static_cast<usize_t>(z) * to.n + x];
            if (u < -0.5 || v < -0.5 || u > size - 0.5 || v > size - 0.5) {
                o = outside;
                continue;
            }
            const auto read = [&](i64_t i, i64_t j) {
                i = std::clamp<i64_t>(i, 0, from.n - 1);
                j = std::clamp<i64_t>(j, 0, from.n - 1);
                return values[static_cast<usize_t>(j) * from.n + static_cast<usize_t>(i)];
            };
            if (nearest) {
                o = read(static_cast<i64_t>(std::floor(u + 0.5)), static_cast<i64_t>(std::floor(v + 0.5)));
                continue;
            }
            const f64_t i  = std::floor(u);
            const f64_t j  = std::floor(v);
            const f64_t fu = u - i;
            const f64_t fv = v - j;
            const i64_t a  = static_cast<i64_t>(i);
            const i64_t b  = static_cast<i64_t>(j);
            o = (read(a, b) * (1.0 - fu) + read(a + 1, b) * fu) * (1.0 - fv)
                + (read(a, b + 1) * (1.0 - fu) + read(a + 1, b + 1) * fu) * fv;
        }
    }
}

namespace {

struct KeptEntry {
    std::pmr::string*        key     = nullptr;
    std::pmr::vector<f64_t>* numbers = nullptr;
};

std::mutex                         g_keptMutex;
std::array<KeptEntry, kKept>       g_kept{};
usize_t                            g_keptNext = 0;

template <typename T>
T* Make() {
    void* storage = memory::General().Allocate(sizeof(T), alignof(T));
    return ::new (storage) T(&memory::General().Resource());
}

template <typename T>
void Destroy(T*& object) noexcept {
    if (object != nullptr) {
        object->~T();
        memory::General().Free(object);
        object = nullptr;
    }
}

} // namespace

void Keep(std::string_view key, std::span<const f64_t> numbers) {
    std::lock_guard lock(g_keptMutex);
    KeptEntry& slot = g_kept[g_keptNext];
    g_keptNext      = (g_keptNext + 1) % kKept;
    if (slot.key == nullptr) {
        slot.key     = Make<std::pmr::string>();
        slot.numbers = Make<std::pmr::vector<f64_t>>();
    }
    slot.key->assign(key);
    slot.numbers->assign(numbers.begin(), numbers.end());
}

b8_t Recall(std::string_view key, std::pmr::vector<f64_t>& out) {
    std::lock_guard lock(g_keptMutex);
    for (const KeptEntry& entry : g_kept) {
        if (entry.key != nullptr && *entry.key == key) {
            out.assign(entry.numbers->begin(), entry.numbers->end());
            return true;
        }
    }
    return false;
}

void ReleaseKept() noexcept {
    std::lock_guard lock(g_keptMutex);
    for (KeptEntry& entry : g_kept) {
        Destroy(entry.key);
        Destroy(entry.numbers);
    }
}

} // namespace engine::terrain::raster
