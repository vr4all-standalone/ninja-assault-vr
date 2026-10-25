// Ninja Assault standalone setup, derived from Confidential Mission and Lupin VR.
package com.flycast.emulator;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** World NJA2 Ver.A cartridge readiness. Plain Java for setup and JVM validation. */
final class NinjaFiles {
    private NinjaFiles() {}
    static final String ZIP = "ninjaslt.zip", BIOS_ZIP = "naomi.zip";
    private static final List<String> CLONES = Arrays.asList("ninjaslta.zip", "ninjasltj.zip", "ninjasltu.zip");
    private static final int DEPTH = 3;
    static final int WALK_LIMIT = 5000;

    static final class Result {
        File zip;
        boolean bios;
        final List<String> missing = new ArrayList<>();
    }
    static Result find(List<File> roots) {
        List<File[]> found = new ArrayList<>();
        for (File root : roots)
            if (root != null) search(root, root, DEPTH, found, new int[] { WALK_LIMIT });
        Result best = null;
        for (File[] f : found) {
            Result r = check(f[0], f[1]);
            if (best == null || r.missing.size() < best.missing.size()) best = r;
            if (r.missing.isEmpty()) return r;
        }
        return best != null ? best : withoutZip(roots);
    }
    static String launchPath(File requested, List<File> roots) {
        Result r;
        if (requested == null) r = find(roots);
        else {
            requested = requested.getAbsoluteFile();
            // Native FindGame is case sensitive; explicit requests must never switch to a sibling.
            if (!requested.getName().equals(ZIP) || !nonEmpty(requested)) return null;
            r = check(requested, requested.getParentFile());
        }
        return r.missing.isEmpty() && r.zip != null ? r.zip.getAbsolutePath() : null;
    }
    private static Result withoutZip(List<File> roots) {
        Result r = new Result();
        File at = null;
        for (File root : roots)
            if (root != null && root.isDirectory()) {
                if (hasBios(new File(root, BIOS_ZIP))) { at = root; r.bios = true; break; }
            }
        String where = at != null ? at.getName() : "Download";
        r.missing.add(ZIP + " (World NJA2 Ver.A cartridge): in " + where + ", next to " + BIOS_ZIP);
        if (!r.bios) r.missing.add(BIOS_ZIP + " (a supported NAOMI BIOS): in " + where + ", next to " + ZIP);
        return r;
    }
    private static void search(File dir, File root, int depth, List<File[]> out, int[] left) {
        if (left[0] <= 0) return;
        File[] files = dir.listFiles();
        if (files == null) return;
        Arrays.sort(files);
        int count = Math.min(left[0], files.length); left[0] -= count;
        for (int i = 0; i < count; i++) {
            File f = files[i];
            String name = f.getName().toLowerCase(java.util.Locale.ROOT);
            if (f.isFile() && (name.equals(ZIP) || CLONES.contains(name))) out.add(new File[] { f, root });
        }
        if (depth > 0)
            for (int i = 0; i < count && left[0] > 0; i++)
                if (!files[i].getName().startsWith(".") && files[i].isDirectory())
                    search(files[i], root, depth - 1, out, left);
    }
    private static Result check(File found, File root) {
        Result r = new Result();
        File dir = found.getParentFile(), exact = new File(dir, ZIP);
        String where = place(dir, root);
        r.zip = exact.isFile() ? exact : found;
        if (!found.getName().equalsIgnoreCase(ZIP))
            r.missing.add(ZIP + ": this app supports World NJA2 Ver.A. Regional clone sets are not yet supported; "
                    + "use your complete World set in " + where + ". Do not rename a regional ZIP.");
        else if (!exact.isFile())
            r.missing.add(ZIP + ": rename the World ZIP to " + ZIP + " in small letters (in " + where + ")");
        else if (!hasCartridge(r.zip))
            r.missing.add(ZIP + ": needs the complete World NJA2 Ver.A cartridge set in a readable ZIP, "
                    + "with all 12 ROM lengths and CRC metadata matching (in " + where + ")");
        BiosStatus separate = inspectBios(new File(dir, BIOS_ZIP)), embedded = inspectBios(r.zip);
        // Native BIOS loading tries named fallbacks across both archives. A corrupt earlier
        // known filename must not shadow a valid later revision in either archive.
        r.bios = separate != BiosStatus.CONFLICT && embedded != BiosStatus.CONFLICT
                && (separate == BiosStatus.READY || embedded == BiosStatus.READY);
        if (!r.bios)
            r.missing.add(BIOS_ZIP + " (a supported 2 MiB NAOMI BIOS with matching CRC metadata): in " + where
                    + ", next to " + ZIP + ", or its BIOS file inside the game ZIP");
        return r;
    }
    private static boolean nonEmpty(File f) { return f.isFile() && f.length() > 0; }
    // World NJA2 Ver.A, core/hw/naomi/naomi_roms.cpp. Native loading finds by CRC first,
    // so a renamed chip is supported only with its expected CRC and uncompressed length.
    private static final long[] ROM_CRCS = { 0xa5bdf581L, 0x1f81f46bL, 0x24974c3dL,
            0x5af34ea0L, 0x504a89b3L, 0xd5c2799aL, 0xcf5df4d3L, 0x5daa6ed4L,
            0xd866cfa8L, 0x1c959b74L, 0x8abed815L, 0xf14d2073L };
    private static final long[] BIOS_CRCS = { 0xd4895685L, 0xd2a1c6bfL, 0x7b452946L,
            0x55413214L, 0xfdf17452L, 0x25f64af7L, 0x33513691L, 0xcce01f1fL };
    private static final String[] BIOS_NAMES = { "epr-21576h.ic27", "epr-21576g.ic27", "epr-21578h.ic27",
            "epr-21578g.ic27", "epr-21577h.ic27", "epr-21577g.ic27", "epr-21579d.ic27", "epr-21576h_multi.ic27" };
    private enum BiosStatus { MISSING, READY, CONFLICT }
    private static boolean hasBios(File archive) { return inspectBios(archive) == BiosStatus.READY; }

    /** Bounded central-directory inspection only. Does not extract, log or read ROM data,
     * and cannot certify payload integrity: a real native boot remains necessary. */
    private static boolean hasCartridge(File archive) {
        boolean[] found = new boolean[ROM_CRCS.length];
        try (ZipFile zip = new ZipFile(archive)) {
            if (zip.size() > WALK_LIMIT) return false;
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (entry.isDirectory()) continue;
                for (int i = 0; i < ROM_CRCS.length; i++)
                    if (entry.getCrc() == ROM_CRCS[i]) {
                        long length = i < 3 ? 0x800000 : 0x1000000;
                        // A bad duplicate can shadow a valid entry in the native CRC lookup.
                        if (entry.getSize() != length || entry.getCompressedSize() <= 0) return false;
                        found[i] = true;
                    }
            }
        } catch (IOException | RuntimeException e) {
            return false;
        }
        for (boolean present : found) if (!present) return false;
        return true;
    }

    private static BiosStatus inspectBios(File archive) {
        boolean ready = false;
        try (ZipFile zip = new ZipFile(archive)) {
            if (zip.size() > WALK_LIMIT) return BiosStatus.CONFLICT;
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (entry.isDirectory()) continue;
                for (int i = 0; i < BIOS_CRCS.length; i++) {
                    boolean validSize = entry.getSize() == 0x200000 && entry.getCompressedSize() > 0;
                    if (entry.getName().equals(BIOS_NAMES[i]) && (entry.getCrc() != BIOS_CRCS[i] || !validSize))
                        return BiosStatus.CONFLICT;
                    if (entry.getCrc() == BIOS_CRCS[i]) {
                        if (!validSize) return BiosStatus.CONFLICT;
                        ready = true;
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            return BiosStatus.MISSING;
        }
        return ready ? BiosStatus.READY : BiosStatus.MISSING;
    }
    private static String place(File dir, File root) {
        String path = dir.getAbsolutePath(), top = root.getAbsolutePath();
        if (path.startsWith(top)) return (root.getName() + path.substring(top.length())).replace(File.separatorChar, '/');
        return dir.getName();
    }
}
