// hotd2-vr: added in 2026 for Ninja Assault VR.
package com.flycast.emulator;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.os.SystemClock;
import android.provider.Settings;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * hotd2-vr: Ninja Assault VR's start screen, the app's entry in the headset's library: a 2D
 * panel, in a process of its own (":setup") so it can end a game still running in the app's main
 * process. It asks for "All files access", checks the game's two files (NinjaFiles) and names what's
 * missing and where it goes (NinjaStartStatus), and Play starts the game in VR (the immersive activity,
 * the alias com.flycast.emulator.MainActivity). The game's quit to the start screen opens it again
 * (BaseGLActivity.quitToStart). It has the look of VR4All, the author's launcher app: its colours, a
 * dark page with a soft glow, rounded panels with thin borders. Built in code, no game art or text.
 */
public class NinjaStartActivity extends Activity
{
    /** The game to start, for the VR activity (a path): the extra VrSetupActivity passed too. */
    public static final String EXTRA_GAME = "ninjaassaultvr.game";

    /** VR4All's colour tokens: the page (deck), panels, their borders (line), muted and cream text,
     *  the signal lime and the violet. */
    static final int DECK = 0xFF0B0D12, PANEL = 0xFF151923, LINE = 0xFF343947, MUTED = 0xFF9BA5B4,
            CREAM = 0xFFF8F5EF, LIME = 0xFFD9F36C, VIOLET = 0xFFA99BFF;
    /** A raised surface inside a panel (file marks, the secondary button), and the lime pressed. */
    private static final int RAISED = 0xFF1B202B, LIME_LIT = 0xFFEDFFA6;

    /** The content's widest (dp): a readable column on a wide panel, centred. */
    private static final int COLUMN_DP = 880;
    private static final int MATCH = LinearLayout.LayoutParams.MATCH_PARENT, WRAP = LinearLayout.LayoutParams.WRAP_CONTENT;

    private TextView badgeText, accessHint, accessStatus, foundAt, chevron;
    private View badgeDot;
    private LinearLayout badge, marksRow, missingList, more;
    private Button allow, play;
    private NinjaStartStatus status;
    /** The player opened or closed "Where to put your files": it then stays as they left it. */
    private boolean moreTouched;

    /** Where the game's files are looked for: the app's own folders (adb), and Download once it may. */
    static List<File> roots(Context context)
    {
        List<File> roots = new ArrayList<>();
        roots.add(new File(context.getFilesDir(), "games"));
        File external = context.getExternalFilesDir(null);
        if (external != null)
            roots.add(new File(external, "games"));
        if (canReadSharedStorage())
            roots.add(new File(Environment.getExternalStorageDirectory(), Environment.DIRECTORY_DOWNLOADS));
        return roots;
    }

    /** The app may look in the headset's shared storage ("All files access"). */
    static boolean canReadSharedStorage()
    {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        getWindow().setBackgroundDrawable(new ColorDrawable(DECK));

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        page.setPadding(dp(40), dp(28), dp(40), dp(32));
        LinearLayout column = new Column(this, dp(COLUMN_DP));
        page.addView(column, new LinearLayout.LayoutParams(MATCH, WRAP));

        column.addView(topBar(), new LinearLayout.LayoutParams(MATCH, WRAP));
        View accent = new View(this);
        GradientDrawable bar = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { LIME, VIOLET, alpha(VIOLET, 0) });
        bar.setCornerRadius(dp(2));
        accent.setBackground(bar);
        column.addView(accent, sized(MATCH, dp(3), 16));

        column.addView(headline("Your next battle.", CREAM), spaced(32));
        column.addView(headline("Inside the headset.", VIOLET), spaced(2));

        column.addView(panel(), sized(MATCH, WRAP, 26));

        String first = "First start:";
        SpannableString firstStart = new SpannableString(first + " prepares your game for VR and uses bundled guns when needed.");
        firstStart.setSpan(new ForegroundColorSpan(CREAM), 0, first.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        column.addView(inset(text(firstStart, 17, MUTED, 500)), spaced(18));

        column.addView(moreToggle(), spaced(6));
        column.addView(more = moreBody(), sized(MATCH, WRAP, 6));

        column.addView(inset(text("Unofficial fan project, not affiliated with Namco or Sega.", 14, MUTED, 500)), spaced(32));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackground(glow());
        scroll.addView(page);
        setContentView(scroll);

        boolean access = canReadSharedStorage();
        show(NinjaStartStatus.from(null, access), null, access);
    }

    @Override
    protected void onResume()
    {
        super.onResume();
        refresh();
    }

    /** The top bar: the NA tile, the app's name and its version. */
    private View topBar()
    {
        LinearLayout bar = row();
        TextView tile = text("NA", 17, DECK, 800);
        tile.setGravity(Gravity.CENTER);
        tile.setIncludeFontPadding(false);
        tile.setBackground(shape(VIOLET, 0, corners(13, 5)));
        bar.addView(tile, new LinearLayout.LayoutParams(dp(46), dp(46)));

        TextView name = text("Ninja Assault VR", 21, CREAM, 700);
        name.setLetterSpacing(-0.01f);
        LinearLayout.LayoutParams grow = new LinearLayout.LayoutParams(0, WRAP, 1);
        grow.leftMargin = dp(16);
        bar.addView(name, grow);

        String v = version();
        if (!v.isEmpty())
        {
            TextView pill = text(v, 15, MUTED, 600);
            pill.setPadding(dp(12), dp(5), dp(12), dp(5));
            pill.setBackground(shape(0, LINE, round(20)));
            bar.addView(pill);
        }
        return bar;
    }

    /** The game's panel: its kind and the status badge, the file marks, what's missing, Play. */
    private View panel()
    {
        LinearLayout panel = box();
        LinearLayout head = row();
        TextView kind = text("ARCADE · RAIL SHOOTER", 14, MUTED, 700);
        kind.setLetterSpacing(0.14f);
        head.addView(kind, new LinearLayout.LayoutParams(0, WRAP, 1));
        badge = row();
        badge.setPadding(dp(14), dp(7), dp(16), dp(7));
        badgeDot = new View(this);
        badge.addView(badgeDot, new LinearLayout.LayoutParams(dp(9), dp(9)));
        badgeText = text("", 15, VIOLET, 700);
        badgeText.setLetterSpacing(0.1f);
        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(WRAP, WRAP);
        gap.leftMargin = dp(9);
        badge.addView(badgeText, gap);
        head.addView(badge);
        panel.addView(head, new LinearLayout.LayoutParams(MATCH, WRAP));

        marksRow = new LinearLayout(this);
        marksRow.setOrientation(narrow() ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        panel.addView(marksRow, spaced(18));

        accessHint = text("Allow it below: the app needs “All files access” to look for the game in "
                + "your Download folder.", 17, CREAM, 500);
        panel.addView(accessHint, spaced(18));

        missingList = new LinearLayout(this);
        missingList.setOrientation(LinearLayout.VERTICAL);
        panel.addView(missingList, sized(MATCH, WRAP, 16));

        play = new Button(this);
        play.setText("▶︎  Play");
        style(play, 22, DECK, shape(LIME, 0xFFEAFF9D, corners(12, 5)), shape(LIME_LIT, 0xFFEAFF9D, corners(12, 5)));
        play.setPadding(dp(44), dp(14), dp(48), dp(14));
        play.setMinWidth(dp(240));
        play.setOnClickListener(v -> play());
        panel.addView(play, spaced(24));
        return panel;
    }

    /** "Where to put your files ›": opens and closes the steps below it. */
    private View moreToggle()
    {
        LinearLayout toggle = row();
        toggle.setPadding(dp(4), dp(10), dp(12), dp(10));
        toggle.setBackground(pressable(shape(0, 0, round(10)), shape(alpha(CREAM, 0x12), 0, round(10))));
        toggle.addView(text("Where to put your files", 18, CREAM, 600));
        chevron = text("›", 26, VIOLET, 700);
        chevron.setGravity(Gravity.CENTER);
        chevron.setIncludeFontPadding(false);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(dp(28), dp(28));
        p.leftMargin = dp(8);
        toggle.addView(chevron, p);
        toggle.setOnClickListener(v -> {
            moreTouched = true;
            setMore(more.getVisibility() != View.VISIBLE);
        });
        return toggle;
    }

    /** The two steps: the access (Allow), and where the game's files go (Look again). */
    private LinearLayout moreBody()
    {
        LinearLayout body = box();
        body.addView(step("01", "Let the app read your files"));
        body.addView(accessStatus = text("", 17, MUTED, 500), spaced(8));
        allow = button("Allow", VIOLET, DECK, 0xFFC4B9FF, 0);
        allow.setOnClickListener(v -> askForAccess());
        body.addView(allow, spaced(14));

        View rule = new View(this);
        rule.setBackgroundColor(LINE);
        body.addView(rule, sized(MATCH, dp(1), 24));

        body.addView(step("02", "Put the game on the headset"), spaced(22));
        body.addView(text("Your own copy of the game, the MAME set, in the headset's Download folder: with "
                + "SideQuest (Manage files), or connect the headset to a PC with USB, allow access in the "
                + "headset, and open Quest > Internal shared storage > Download. Nothing of the game comes "
                + "with this app. Use the complete World NJA2 Ver.A cartridge set, ninjaslt.zip, and naomi.zip "
                + "beside it (or a supported BIOS inside the game ZIP). Keep the ZIP packed. Regional "
                + "clone sets are not yet supported; do not rename them to ninjaslt.zip.", 17, MUTED, 500), spaced(8));
        body.addView(foundAt = text("", 17, CREAM, 500), spaced(10));
        Button again = button("Look again", RAISED, CREAM, 0xFF252B37, 0xFF464D55);
        again.setOnClickListener(v -> refresh());
        body.addView(again, spaced(16));
        body.setVisibility(View.GONE);
        return body;
    }

    private void setMore(boolean open)
    {
        more.setVisibility(open ? View.VISIBLE : View.GONE);
        chevron.setRotation(open ? 90 : 0);
    }

    private NinjaFiles.Result refresh()
    {
        boolean access = canReadSharedStorage();
        NinjaFiles.Result ninja = NinjaFiles.find(roots(this));
        show(NinjaStartStatus.from(ninja, access), ninja, access);
        return ninja;
    }

    /** The screen for a status: the badge, a mark per file, a line per missing file, Play, the steps. */
    private void show(NinjaStartStatus s, NinjaFiles.Result ninja, boolean access)
    {
        status = s;
        int tone = s.ready ? LIME : VIOLET;
        badgeText.setText(s.badge);
        badgeText.setTextColor(tone);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(tone);
        badgeDot.setBackground(dot);
        badge.setBackground(shape(alpha(tone, 0x1F), alpha(tone, 0x73), round(20)));

        // Before the access, Download hasn't been looked in: no marks or missing lines, just the hint;
        // unless the files were all found in the app's own folder (READY without the access).
        boolean looked = access || s.ready;
        marksRow.removeAllViews();
        boolean horizontal = marksRow.getOrientation() == LinearLayout.HORIZONTAL;
        for (int i = 0; i < s.fileMarks.size(); i++)
        {
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(WRAP, WRAP);
            if (i > 0)
            {
                if (horizontal)
                    p.leftMargin = dp(10);
                else
                    p.topMargin = dp(8);
            }
            marksRow.addView(mark(s.fileMarks.get(i)), p);
        }
        marksRow.setVisibility(looked && !s.fileMarks.isEmpty() ? View.VISIBLE : View.GONE);
        missingList.removeAllViews();
        for (int i = 0; i < s.missing.size(); i++)
            missingList.addView(missingLine(s.missing.get(i)), spaced(i == 0 ? 0 : 10));
        missingList.setVisibility(looked && !s.missing.isEmpty() ? View.VISIBLE : View.GONE);
        accessHint.setVisibility(looked ? View.GONE : View.VISIBLE);

        play.setEnabled(s.ready);
        play.setAlpha(s.ready ? 1f : 0.35f);

        if (access)
        {
            accessStatus.setText("Done: the app can look in your Download folder.");
            accessStatus.setTextColor(LIME);
            allow.setVisibility(View.GONE);
        }
        else
        {
            accessStatus.setText("The app looks for the game in the headset's Download folder. For that it needs "
                    + "\"All files access\": tap Allow, switch it on, and come back here.");
            accessStatus.setTextColor(MUTED);
            allow.setVisibility(View.VISIBLE);
        }
        if (ninja != null && ninja.zip != null)
        {
            foundAt.setText("Found: " + shortPath(ninja.zip));
            foundAt.setVisibility(View.VISIBLE);
        }
        else
            foundAt.setVisibility(View.GONE);
        // open while something's left to do, until the player opens or closes it themselves
        if (!moreTouched)
            setMore(!s.ready);
    }

    /** A file's mark: "ninjaslt.zip ✓" (the tick in lime), or "naomi.zip missing" in violet. */
    private TextView mark(String mark)
    {
        boolean found = mark.endsWith(NinjaStartStatus.FOUND);
        SpannableString s = new SpannableString(mark);
        if (found)
            s.setSpan(new ForegroundColorSpan(LIME), mark.length() - 1, mark.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView t = text(s, 18, found ? CREAM : VIOLET, 600);
        t.setPadding(dp(14), dp(8), dp(16), dp(8));
        t.setBackground(found ? shape(RAISED, LINE, round(10))
                : shape(alpha(VIOLET, 0x1F), alpha(VIOLET, 0x80), round(10)));
        return t;
    }

    /** "naomi.zip missing" in bold violet, then where it goes. */
    private TextView missingLine(String line)
    {
        int cut = line.indexOf(NinjaStartStatus.MISSING) + NinjaStartStatus.MISSING.length();
        SpannableString s = new SpannableString(line);
        s.setSpan(new ForegroundColorSpan(VIOLET), 0, cut, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        s.setSpan(new StyleSpan(Typeface.BOLD), 0, cut, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        return text(s, 17, MUTED, 500);
    }

    private void askForAccess()
    {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
            return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void play()
    {
        // the files as they are now (one may have gone since the screen was drawn)
        NinjaFiles.Result ninja = refresh();
        if (!status.ready)
            return;
        // A game may still be running in the app's own process (the app opened again from the
        // library while one played): the emulator has one game per process, and its task would
        // only be brought back. So that process ends, and the game starts in a new one.
        endGameProcess();
        // the immersive activity, which starts in VR
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.setComponent(new ComponentName(this, "com.flycast.emulator.MainActivity"));
        intent.putExtra(EXTRA_GAME, ninja.zip.getAbsolutePath());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();
    }

    /** Ends the app's main process (this screen runs in one of its own), and waits for it. */
    private void endGameProcess()
    {
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null)
            return;
        for (ActivityManager.RunningAppProcessInfo p : running)
        {
            if (p.pid == Process.myPid() || !getPackageName().equals(p.processName))
                continue;
            Process.killProcess(p.pid);
            for (int i = 0; i < 50 && new File("/proc/" + p.pid).exists(); i++)
                SystemClock.sleep(20);
        }
    }

    /** "v1.1": the app's versionName (the vr build's is "<hubVersion>-g<commit>"); "" without one. */
    private String version()
    {
        try {
            return NinjaStartStatus.shortVersion(getPackageManager().getPackageInfo(getPackageName(), 0).versionName);
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    private String shortPath(File f)
    {
        String path = f.getAbsolutePath();
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        return path.startsWith(root + "/") ? path.substring(root.length() + 1) : f.getName();
    }

    /** The page's background: the deck, a violet glow top right and a dim teal one bottom left. */
    private Drawable glow()
    {
        return new LayerDrawable(new Drawable[] { new ColorDrawable(DECK),
                radial(alpha(VIOLET, 0x26), 0.74f, 0f, 560), radial(0x40253D3B, 0.12f, 1f, 480) });
    }

    private GradientDrawable radial(int colour, float x, float y, int radiusDp)
    {
        GradientDrawable g = new GradientDrawable();
        g.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        g.setColors(new int[] { colour, alpha(colour, 0) });
        g.setGradientCenter(x, y);
        g.setGradientRadius(dp(radiusDp));
        return g;
    }

    private TextView headline(String s, int colour)
    {
        TextView t = text(s, 38, colour, 800);
        t.setLetterSpacing(-0.02f);
        t.setLineSpacing(0, 1f);
        t.setIncludeFontPadding(false);
        return t;
    }

    /** A step's heading in the steps' box: its number in lime, then its title. */
    private View step(String number, String title)
    {
        LinearLayout r = row();
        TextView n = text(number, 15, LIME, 800);
        n.setLetterSpacing(0.08f);
        r.addView(n);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(WRAP, WRAP);
        p.leftMargin = dp(12);
        r.addView(text(title, 19, CREAM, 700), p);
        return r;
    }

    private TextView text(CharSequence s, float sp, int colour, int weight)
    {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(colour);
        t.setTypeface(Typeface.create(Typeface.SANS_SERIF, weight, false));
        t.setLineSpacing(0, 1.2f);
        return t;
    }

    /** Text lined up with the panels' rounded edge. */
    private TextView inset(TextView t)
    {
        t.setPadding(dp(4), 0, dp(4), 0);
        return t;
    }

    private Button button(String label, int fill, int textColour, int pressedFill, int stroke)
    {
        Button b = new Button(this);
        b.setText(label);
        style(b, 18, textColour, shape(fill, stroke, round(10)), shape(pressedFill, stroke, round(10)));
        b.setPadding(dp(28), dp(11), dp(28), dp(11));
        return b;
    }

    private void style(Button b, float sp, int textColour, Drawable normal, Drawable lit)
    {
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        b.setTextColor(textColour);
        b.setTypeface(Typeface.create(Typeface.SANS_SERIF, 700, false));
        b.setGravity(Gravity.CENTER);
        b.setStateListAnimator(null);
        b.setBackground(pressable(normal, lit));
    }

    /** Lit while pressed or under the headset's pointer. */
    private static StateListDrawable pressable(Drawable normal, Drawable lit)
    {
        StateListDrawable d = new StateListDrawable();
        d.addState(new int[] { android.R.attr.state_pressed }, lit);
        d.addState(new int[] { android.R.attr.state_hovered }, lit);
        d.addState(new int[0], normal);
        return d;
    }

    /** A panel: the panel colour, a 16 dp radius and a 1 dp line border, padded. */
    private LinearLayout box()
    {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(28), dp(24), dp(28), dp(26));
        box.setBackground(shape(PANEL, LINE, round(16)));
        return box;
    }

    private LinearLayout row()
    {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        return r;
    }

    /** A rounded shape: fill (0: none), a 1 dp stroke (0: none), corner radii in dp. */
    private GradientDrawable shape(int fill, int stroke, float[] radiiDp)
    {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        if (stroke != 0)
            g.setStroke(dp(1), stroke);
        float[] radii = new float[8];
        for (int i = 0; i < 8; i++)
            radii[i] = dp(radiiDp[i]);
        g.setCornerRadii(radii);
        return g;
    }

    private static float[] round(float r)
    {
        return new float[] { r, r, r, r, r, r, r, r };
    }

    /** VR4All's cut corners: top left and bottom right round by a, the other two by b. */
    private static float[] corners(float a, float b)
    {
        return new float[] { a, a, b, b, a, a, b, b };
    }

    private static int alpha(int colour, int a)
    {
        return (colour & 0x00FFFFFF) | (a << 24);
    }

    /** The file marks go one under another when the column can't fit both side by side. */
    private boolean narrow()
    {
        DisplayMetrics m = getResources().getDisplayMetrics();
        float width = Math.min(m.widthPixels / m.density - 80, COLUMN_DP);
        return width - 56 < 600;
    }

    private LinearLayout.LayoutParams spaced(int top)
    {
        return sized(WRAP, WRAP, top);
    }

    private LinearLayout.LayoutParams sized(int width, int height, int top)
    {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(width, height);
        p.topMargin = dp(top);
        return p;
    }

    private int dp(float v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    /** A vertical column no wider than maxWidth (px); its parent centres it. */
    private static final class Column extends LinearLayout
    {
        private final int maxWidth;

        Column(Context context, int maxWidth)
        {
            super(context);
            this.maxWidth = maxWidth;
            setOrientation(VERTICAL);
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec)
        {
            if (MeasureSpec.getMode(widthSpec) != MeasureSpec.UNSPECIFIED && MeasureSpec.getSize(widthSpec) > maxWidth)
                widthSpec = MeasureSpec.makeMeasureSpec(maxWidth, MeasureSpec.EXACTLY);
            super.onMeasure(widthSpec, heightSpec);
        }
    }
}
