package com.flycast.emulator;

import static org.junit.Assert.*;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Synthetic ZIP headers exercise metadata checks without including game or BIOS data. */
public class NinjaFilesTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();
    private File games, download;

    // Independent literals from the World NJA2 Ver.A definition in naomi_roms.cpp.
    private static final String[] NAMES = { "nja2vera.2d", "nja1vera.2c", "nja1vera.2b", "nja1ma2.4l",
        "nja1ma3.4k", "nja1ma4.4j", "nja1ma5.4h", "nja1ma6.4f", "nja1ma7.4e", "nja1ma8.4d",
        "nja1ma9.4c", "nja1ma10.4b" };
    private static final long[] CRCS = { 0xa5bdf581L, 0x1f81f46bL, 0x24974c3dL, 0x5af34ea0L,
        0x504a89b3L, 0xd5c2799aL, 0xcf5df4d3L, 0x5daa6ed4L, 0xd866cfa8L, 0x1c959b74L,
        0x8abed815L, 0xf14d2073L };

    @Before public void setup() throws IOException {
        games = tmp.newFolder("games");
        download = tmp.newFolder("Download");
    }
    private List<File> roots() { return Arrays.asList(games, download); }
    private static final class Entry {
        String name; long size, crc;
        Entry(String name, long size, long crc) { this.name = name; this.size = size; this.crc = crc; }
    }
    private List<Entry> cartridge() {
        List<Entry> entries = new ArrayList<>();
        for (int i = 0; i < NAMES.length; i++)
            entries.add(new Entry(NAMES[i], i < 3 ? 0x800000 : 0x1000000, CRCS[i]));
        return entries;
    }
    private Entry bios() { return new Entry("epr-21578h.ic27", 0x200000, 0x7b452946L); }
    private void bios(File dir) throws IOException { zip(new File(dir, "naomi.zip"), Arrays.asList(bios())); }
    private File complete(File dir) throws IOException {
        File game = new File(dir, "ninjaslt.zip");
        zip(game, cartridge()); bios(dir); return game;
    }
    private void blocked(File game) {
        assertNull("Invalid explicit launch must not choose another set", NinjaFiles.launchPath(game, roots()));
        assertFalse(NinjaStartStatus.from(NinjaFiles.find(Arrays.asList(game.getParentFile())), true).ready);
    }

    @Test public void completeWorldCartridgeAndBiosLaunchWithoutDisc() throws IOException {
        File game = complete(download);
        assertEquals(game.getAbsolutePath(), NinjaFiles.launchPath(null, roots()));
        assertTrue(NinjaFiles.find(roots()).missing.toString(), NinjaFiles.find(roots()).missing.isEmpty());
    }
    @Test public void missingGameAndBiosBlockPlay() {
        assertNull(NinjaFiles.launchPath(null, roots()));
        assertEquals(2, NinjaFiles.find(roots()).missing.size());
    }
    @Test public void missingBiosBlocksCompleteCartridge() throws IOException {
        File game = new File(download, "ninjaslt.zip"); zip(game, cartridge()); blocked(game);
    }
    @Test public void biosMayBeEmbeddedInGameArchive() throws IOException {
        List<Entry> entries = cartridge(); entries.add(bios());
        File game = new File(download, "ninjaslt.zip"); zip(game, entries);
        assertEquals(game.getAbsolutePath(), NinjaFiles.launchPath(game, roots()));
    }
    @Test public void malformedArchiveBlocksPlay() throws IOException {
        File game = new File(download, "ninjaslt.zip");
        try (FileOutputStream out = new FileOutputStream(game)) { out.write(new byte[] {1, 2, 3, 4}); }
        bios(download); blocked(game);
    }
    @Test public void emptyArchiveBlocksPlay() throws IOException {
        File game = new File(download, "ninjaslt.zip"); assertTrue(game.createNewFile());
        bios(download); blocked(game);
    }
    @Test public void missingOneCartridgeChipBlocksPlay() throws IOException {
        List<Entry> entries = cartridge(); entries.remove(11);
        File game = new File(download, "ninjaslt.zip"); zip(game, entries); bios(download); blocked(game);
    }
    @Test public void expectedNameWithWrongSizeBlocksPlay() throws IOException {
        List<Entry> entries = cartridge(); entries.get(0).size = 1;
        File game = new File(download, "ninjaslt.zip"); zip(game, entries); bios(download); blocked(game);
    }
    @Test public void expectedNameWithWrongCrcBlocksPlay() throws IOException {
        List<Entry> entries = cartridge(); entries.get(0).crc = 0xc4c60b4cL; // Japan revision.
        File game = new File(download, "ninjaslt.zip"); zip(game, entries); bios(download); blocked(game);
    }
    @Test public void renamedEntryWithMatchingCrcAndSizeUsesNativeLookup() throws IOException {
        List<Entry> entries = cartridge(); entries.get(0).name = "different-name.bin";
        File game = new File(download, "ninjaslt.zip"); zip(game, entries); bios(download);
        assertEquals(game.getAbsolutePath(), NinjaFiles.launchPath(game, roots()));
    }
    @Test public void invalidRequestedSetNeverFallsThroughToCompleteSet() throws IOException {
        complete(games);
        File bad = new File(download, "ninjaslt.zip"); zip(bad, Arrays.asList(bios()));
        assertNull(NinjaFiles.launchPath(bad, roots()));
    }
    @Test public void regionalCloneNeedsAnExplicitlySupportedRevision() throws IOException {
        File parent = complete(download);
        File clone = new File(download, "ninjasltj.zip"); zip(clone, cartridge());
        assertNull(NinjaFiles.launchPath(clone, roots()));
        assertEquals(parent.getAbsolutePath(), NinjaFiles.launchPath(null, roots()));
    }
    @Test public void cloneOnlyExplainsWorldRequirement() throws IOException {
        zip(new File(download, "ninjasltj.zip"), cartridge()); bios(download);
        assertNull(NinjaFiles.launchPath(null, roots()));
        assertTrue(NinjaFiles.find(roots()).missing.toString(),
                NinjaFiles.find(roots()).missing.get(0).contains("World"));
    }
    @Test public void unsupportedBiosCrcBlocksPlayEvenWithExpectedName() throws IOException {
        File game = complete(download);
        Entry wrong = bios(); wrong.crc = 123;
        zip(new File(download, "naomi.zip"), Arrays.asList(wrong)); blocked(game);
    }
    @Test public void wrongBiosSizeBlocksPlay() throws IOException {
        File game = complete(download);
        Entry wrong = bios(); wrong.size = 1;
        zip(new File(download, "naomi.zip"), Arrays.asList(wrong)); blocked(game);
    }
    @Test public void supportedBiosMayBeRenamedByItsCrc() throws IOException {
        File game = complete(download);
        Entry renamed = bios(); renamed.name = "renamed-bios.bin";
        zip(new File(download, "naomi.zip"), Arrays.asList(renamed));
        assertEquals(game.getAbsolutePath(), NinjaFiles.launchPath(game, roots()));
    }
    @Test public void completeNestedSetWinsOverIncompleteEarlierRoot() throws IOException {
        zip(new File(games, "ninjaslt.zip"), Arrays.asList(bios()));
        File nested = new File(download, "Ninja"); assertTrue(nested.mkdir());
        File game = complete(nested);
        assertEquals(game.getAbsolutePath(), NinjaFiles.launchPath(null, roots()));
    }
    @Test public void readyStatusHasTwoMarksAndOwnStorageNeedsNoAccess() throws IOException {
        complete(games);
        NinjaStartStatus s = NinjaStartStatus.from(NinjaFiles.find(roots()), false);
        assertTrue(s.ready); assertEquals("READY", s.badge);
        assertEquals(Arrays.asList("ninjaslt.zip ✓", "naomi.zip ✓"), s.fileMarks);
        assertTrue(s.missing.isEmpty());
    }
    @Test public void missingWithoutSharedStorageAccessAsksForAccess() {
        NinjaStartStatus s = NinjaStartStatus.from(NinjaFiles.find(roots()), false);
        assertFalse(s.ready); assertEquals("ALLOW ACCESS", s.badge);
    }
    @Test public void initialStatusDoesNotEnablePlay() {
        NinjaStartStatus s = NinjaStartStatus.from(null, true);
        assertFalse(s.ready); assertEquals("CHECKING", s.badge); assertTrue(s.fileMarks.isEmpty());
    }

    @Test public void missingFilesInstructionsPointToDownload() {
        NinjaFiles.Result r = NinjaFiles.find(roots());
        assertTrue(r.missing.get(0), r.missing.get(0).contains("in Download"));
        assertTrue(r.missing.get(1), r.missing.get(1).contains("in Download"));
    }
    @Test public void missingBiosStatusNamesOnlyBios() throws IOException {
        zip(new File(download, "ninjaslt.zip"), cartridge());
        NinjaStartStatus s = NinjaStartStatus.from(NinjaFiles.find(roots()), true);
        assertFalse(s.ready); assertEquals("MISSING FILES", s.badge);
        assertEquals(Arrays.asList("ninjaslt.zip ✓", "naomi.zip missing"), s.fileMarks);
        assertEquals(1, s.missing.size()); assertTrue(s.missing.get(0).startsWith("naomi.zip missing"));
    }
    @Test public void malformedBiosBlocksPlay() throws IOException {
        File game = complete(download);
        try (FileOutputStream out = new FileOutputStream(new File(download, "naomi.zip"))) { out.write(1); }
        blocked(game);
    }
    @Test public void emptyCentralDirectoryBlocksPlay() throws IOException {
        File game = new File(download, "ninjaslt.zip"); zip(game, new ArrayList<>()); bios(download); blocked(game);
    }
    @Test public void truncatedArchiveBlocksPlay() throws IOException {
        File game = complete(download);
        try (java.io.RandomAccessFile out = new java.io.RandomAccessFile(game, "rw")) { out.setLength(game.length() - 12); }
        blocked(game);
    }
    @Test public void invalidDuplicateCrcDoesNotShadowValidChip() throws IOException {
        List<Entry> entries = cartridge(); entries.add(0, new Entry("bad-first.bin", 1, 0xa5bdf581L));
        File game = new File(download, "ninjaslt.zip"); zip(game, entries); bios(download); blocked(game);
    }
    @Test public void unrelatedExplicitGameCannotUseFoundWorldSet() throws IOException {
        complete(games);
        File other = new File(download, "confmiss.zip"); zip(other, cartridge());
        assertNull(NinjaFiles.launchPath(other, roots()));
    }
    @Test public void versionBadgeKeepsMajorMinor() {
        assertEquals("v1.0", NinjaStartStatus.shortVersion("1.0.0-gabc123"));
        assertEquals("v10.4", NinjaStartStatus.shortVersion("10.4.2-gabc-vr"));
        assertEquals("", NinjaStartStatus.shortVersion(null));
        assertEquals("", NinjaStartStatus.shortVersion("gabc123"));
    }

    @Test public void explicitNoncanonicalNameCannotSubstituteCanonicalSet() throws IOException {
        complete(download);
        assertNull(NinjaFiles.launchPath(new File(download, "NINJASLT.ZIP"), roots()));
    }
    @Test public void invalidEarlierBiosFilenameCannotShadowValidLaterRevision() throws IOException {
        File game = complete(download);
        zip(new File(download, "naomi.zip"), Arrays.asList(
                new Entry("epr-21577h.ic27", 0x200000, 123),
                new Entry("epr-21577g.ic27", 0x200000, 0x25f64af7L)));
        blocked(game);
    }
    @Test public void invalidEmbeddedBiosCannotShadowSeparateValidRevision() throws IOException {
        List<Entry> entries = cartridge(); entries.add(new Entry("epr-21577h.ic27", 0x200000, 123));
        File game = new File(download, "ninjaslt.zip"); zip(game, entries);
        zip(new File(download, "naomi.zip"), Arrays.asList(new Entry("epr-21577g.ic27", 0x200000, 0x25f64af7L)));
        blocked(game);
    }

    // Only the directory and local-header metadata are used by setup. The one-byte stand-in
    // payloads are deliberately not ROMs; these fixtures do not test full content integrity.
    private void zip(File file, List<Entry> entries) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        List<Integer> offsets = new ArrayList<>();
        for (Entry e : entries) {
            offsets.add(bytes.size()); byte[] name = e.name.getBytes(StandardCharsets.US_ASCII);
            le(out, 0x04034b50, 4); le(out, 20, 2); le(out, 0, 2); le(out, 8, 2);
            le(out, 0, 4); le(out, e.crc, 4); le(out, 1, 4); le(out, e.size, 4);
            le(out, name.length, 2); le(out, 0, 2); out.write(name); out.write(0);
        }
        int central = bytes.size();
        for (int i = 0; i < entries.size(); i++) {
            Entry e = entries.get(i); byte[] name = e.name.getBytes(StandardCharsets.US_ASCII);
            le(out, 0x02014b50, 4); le(out, 20, 2); le(out, 20, 2); le(out, 0, 2); le(out, 8, 2);
            le(out, 0, 4); le(out, e.crc, 4); le(out, 1, 4); le(out, e.size, 4);
            le(out, name.length, 2); le(out, 0, 2); le(out, 0, 2); le(out, 0, 2);
            le(out, 0, 2); le(out, 0, 4); le(out, offsets.get(i), 4); out.write(name);
        }
        int length = bytes.size() - central;
        le(out, 0x06054b50, 4); le(out, 0, 2); le(out, 0, 2);
        le(out, entries.size(), 2); le(out, entries.size(), 2);
        le(out, length, 4); le(out, central, 4); le(out, 0, 2);
        try (FileOutputStream fileOut = new FileOutputStream(file)) { bytes.writeTo(fileOut); }
    }
    private void le(DataOutputStream out, long n, int length) throws IOException {
        for (int i = 0; i < length; i++) out.write((int) (n >>> (8 * i)) & 0xff);
    }
}
