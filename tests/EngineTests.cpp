// Self-contained tests for the repair engine. Exit code 0 means all passed.
#include "../engine/Fft.h"
#include "../engine/SpectralRepair.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
const double pi = 3.14159265358979323846;
int failures = 0;

void check (bool ok, const char* name, double value)
{
    std::printf ("%s  %-58s %10.2f\n", ok ? "PASS" : "FAIL", name, value);
    if (! ok)
        ++failures;
}

double rmsDb (const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
    {
        const double d = (double) a[i] - (double) b[i];
        sum += d * d;
    }
    return 10.0 * std::log10 (sum / (double) (to - from) + 1.0e-20);
}

// Music-like bed: a steady chord plus a little noise.
std::vector<float> makeBed (double sampleRate, std::size_t n)
{
    std::mt19937 rng (7);
    std::normal_distribution<double> noise (0.0, 0.002);
    std::vector<float> x (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate;
        x[i] = (float) (0.25 * std::sin (2 * pi * 220.0 * t) + 0.15 * std::sin (2 * pi * 554.4 * t)
                        + 0.10 * std::sin (2 * pi * 2637.0 * t) + noise (rng));
    }
    return x;
}

void run (double sampleRate, int fftSize)
{
    std::printf ("\n-- %.0f Hz, FFT %d --\n", sampleRate, fftSize);
    const auto n = (std::size_t) (sampleRate * 2.0);
    const auto clean = makeBed (sampleRate, n);

    // Disturbance: a 60 ms "squeak" at 3.1-3.6 kHz, overlapping the 2637 Hz partial's band.
    auto damaged = clean;
    const auto b0 = (std::size_t) (sampleRate * 1.00), b1 = (std::size_t) (sampleRate * 1.06);
    for (auto i = b0; i < b1; ++i)
    {
        const double t = (double) (i - b0) / sampleRate;
        const double env = std::sin (pi * (double) (i - b0) / (double) (b1 - b0));
        damaged[i] += (float) (0.3 * env * std::sin (2 * pi * (3100.0 + 4000.0 * t) * t));
    }

    mender::Selection sel;
    sel.startSample = (std::int64_t) (sampleRate * 0.99);
    sel.endSample = (std::int64_t) (sampleRate * 1.07);
    sel.lowHz = 2000.0;
    sel.highHz = 5000.0;

    mender::Settings settings;
    settings.fftSize = fftSize;

    auto repaired = damaged;
    float* chans[] = { repaired.data() };
    const auto r = mender::repair (chans, 1, (std::int64_t) n, sampleRate, sel, settings);
    const auto range = mender::affectedRange ((std::int64_t) n, sel, settings);

    check (r.changed && r.changedStart == range.changedStart && r.changedEnd == range.changedEnd,
           "reports the range it changed", (double) (r.changedEnd - r.changedStart));

    bool identical = true;
    for (std::size_t i = 0; i < n; ++i)
        if (((std::int64_t) i < r.changedStart || (std::int64_t) i >= r.changedEnd) && repaired[i] != damaged[i])
            identical = false;
    check (identical, "audio outside the changed range is bit-identical", 0.0);

    const double errBefore = rmsDb (damaged, clean, b0, b1);
    const double errAfter = rmsDb (repaired, clean, b0, b1);
    check (errAfter < errBefore - 20.0, "disturbance reduced by more than 20 dB (dB removed)", errBefore - errAfter);

    // Repairing clean audio should do almost nothing.
    auto untouched = clean;
    float* chans2[] = { untouched.data() };
    mender::repair (chans2, 1, (std::int64_t) n, sampleRate, sel, settings);
    const double cleanErr = rmsDb (untouched, clean, (std::size_t) range.changedStart, (std::size_t) range.changedEnd);
    const double signalDb = rmsDb (clean, std::vector<float> (n, 0.0f), b0, b1);
    check (cleanErr < signalDb - 40.0, "clean audio changes by less than -40 dB relative", cleanErr - signalDb);

    // Frequency reference flattens everything in the box to the level of the
    // bands above and below, so use a box that holds only the disturbance.
    auto repairedF = damaged;
    float* chans3[] = { repairedF.data() };
    settings.reference = mender::Reference::frequency;
    auto selF = sel;
    selF.lowHz = 2900.0;
    mender::repair (chans3, 1, (std::int64_t) n, sampleRate, selF, settings);
    const double errF = rmsDb (repairedF, clean, b0, b1);
    check (errF < errBefore - 10.0, "frequency reference removes more than 10 dB (dB removed)", errBefore - errF);

    // Strength 0 is a no-op.
    auto same = damaged;
    float* chans4[] = { same.data() };
    settings.strength = 0.0;
    check (! mender::repair (chans4, 1, (std::int64_t) n, sampleRate, sel, settings).changed && same == damaged,
           "strength 0 leaves the audio untouched", 0.0);
}

void edgeCases()
{
    std::printf ("\n-- edge cases --\n");
    const double sr = 48000.0;
    auto x = makeBed (sr, 6000);
    const auto original = x;
    float* chans[] = { x.data() };

    mender::Selection sel { 0, 6000, 1000.0, 4000.0 }; // whole clip: no time reference available
    const auto r = mender::repair (chans, 1, 6000, sr, sel, {});
    bool finite = true;
    for (auto v : x)
        finite = finite && std::isfinite (v);
    check (finite && r.changedStart == 0 && r.changedEnd == 6000, "box covering the whole clip stays in bounds", 0.0);

    x = original;
    mender::Selection empty { 3000, 3000, 1000.0, 4000.0 };
    check (! mender::repair (chans, 1, 6000, sr, empty, {}).changed && x == original, "empty selection is a no-op", 0.0);

    mender::Selection outside { -500, 99999, 0.0, 30000.0 };
    mender::repair (chans, 1, 6000, sr, outside, {});
    finite = true;
    for (auto v : x)
        finite = finite && std::isfinite (v);
    check (finite, "out-of-range selection is clamped", 0.0);

    // FFT round trip.
    mender::Fft fft (1024);
    std::vector<std::complex<double>> d (1024);
    for (std::size_t i = 0; i < d.size(); ++i)
        d[i] = std::sin (0.37 * (double) i) + 0.2 * std::cos (1.9 * (double) i);
    const auto ref = d;
    fft.forward (d);
    fft.inverse (d);
    double err = 0.0;
    for (std::size_t i = 0; i < d.size(); ++i)
        err = std::max (err, std::abs (d[i] - ref[i]));
    check (err < 1.0e-12, "FFT forward then inverse returns the input", err);
}
} // namespace

int main()
{
    run (44100.0, 2048);
    run (48000.0, 2048);
    run (96000.0, 4096);
    run (48000.0, 8192);
    edgeCases();
    std::printf ("\n%s\n", failures == 0 ? "All tests passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
