

# CBR: High quality settings (e.g, -q 0) degrade quality over -q 4

## A high quality MP3 encoder

Brought to you by:
[aleidinger](/u/aleidinger/profile/),
[bouvigne](/u/bouvigne/profile/),
[jaz001](/u/jaz001/profile/),
[rbrito](/u/rbrito/profile/),
[robert](/u/robert/profile/)

* [Summary](/projects/lame/)
* [Files](/projects/lame/files/)
* [Reviews](/projects/lame/reviews/)
* [Support](/projects/lame/support)
* [Mailing Lists](/p/lame/mailman/)
* [Tickets ▾](/p/lame/_list/tickets)
  + [Bugs](/p/lame/bugs/)
  + [Support Requests](/p/lame/support-requests/)
  + [Patches](/p/lame/patches/)
  + [Feature Requests](/p/lame/feature-requests/)
* [News](/p/lame/news/)
* [Discussion](/p/lame/discussion/)
* [Code (SVN)](/p/lame/svn/)
* [Code (old CVS)](/p/lame/code/)

Menu
▾
▴

* [Create Ticket](/p/lame/bugs/new/ "To create a new ticket, you must be authorized by the project admin.")
* [View Stats](/p/lame/bugs/stats/ "Stats")

### Group

* [Compatibility](/p/lame/bugs/milestone/Compatibility/)
* [Portability](/p/lame/bugs/milestone/Portability/)
* [Quality](/p/lame/bugs/milestone/Quality/)
* [Speed](/p/lame/bugs/milestone/Speed/)
* [Unknown](/p/lame/bugs/milestone/Unknown/)
* [Usability](/p/lame/bugs/milestone/Usability/)

### Searches

* [Changes](/p/lame/bugs/search/?q=%21status%3Aclosed-rejected+%26%26+%21status%3Aclosed-invalid+%26%26+%21status%3Aclosed-later+%26%26+%21status%3Aclosed-duplicate+%26%26+%21status%3Aclosed-out-of-date+%26%26+%21status%3Aclosed-postponed+%26%26+%21status%3Aclosed-accepted+%26%26+%21status%3Aclosed-remind+%26%26+%21status%3Aclosed-works-for-me+%26%26+%21status%3Aclosed+%26%26+%21status%3Aclosed-wont-fix+%26%26+%21status%3Aclosed-fixed&sort=mod_date_dt+desc)
* [Closed Tickets](/p/lame/bugs/search/?q=status%3Aclosed-rejected+or+status%3Aclosed-invalid+or+status%3Aclosed-later+or+status%3Aclosed-duplicate+or+status%3Aclosed-out-of-date+or+status%3Aclosed-postponed+or+status%3Aclosed-accepted+or+status%3Aclosed-remind+or+status%3Aclosed-works-for-me+or+status%3Aclosed+or+status%3Aclosed-wont-fix+or+status%3Aclosed-fixed)
* [Open Tickets](/p/lame/bugs/search/?q=%21status%3Aclosed-rejected+%26%26+%21status%3Aclosed-invalid+%26%26+%21status%3Aclosed-later+%26%26+%21status%3Aclosed-duplicate+%26%26+%21status%3Aclosed-out-of-date+%26%26+%21status%3Aclosed-postponed+%26%26+%21status%3Aclosed-accepted+%26%26+%21status%3Aclosed-remind+%26%26+%21status%3Aclosed-works-for-me+%26%26+%21status%3Aclosed+%26%26+%21status%3Aclosed-wont-fix+%26%26+%21status%3Aclosed-fixed)

### Help

* [Formatting Help](/nf/markdown_syntax)

## #516 CBR: High quality settings (e.g, -q 0) degrade quality over -q 4

Milestone:
[Compatibility](/p/lame/bugs/milestone/Compatibility)

Status:
open

Owner:
nobody

Labels:
None

Priority:
5

Updated:

2025-05-28

Created:

2024-06-21

Creator:
[maikmerten](/u/maikmerten/profile/)

Private:
No

When encoding files in CBR mode (potentially ABR as well, yet to be properly explored), quality for various audio samples degrade when selecting higher quality settings than `-q 4`.

There's a discussion over at HydrogenAudio. I summarized my findings in following posts:

[https://hydrogenaud.io/index.php/topic,126120.msg1046452.html#msg1046452](https://hydrogenaud.io/index.php/topic%2C126120.msg1046452.html#msg1046452) (in short: `noise_shaping_amp` makes the difference between `-q 0` and `-q 4`)

[https://hydrogenaud.io/index.php/topic,126120.msg1046480.html#msg1046480](https://hydrogenaud.io/index.php/topic%2C126120.msg1046480.html#msg1046480) (in short: The difference between `-q 0` and `-q 4` is easily ABXable at 128 kbps, with `-q 0` showing obvious artifacting)

My current hypothesis on what may be happening: If `noise_shaping_amp` is set to `1` or `2` (higher quality settings), only one or some bands will receive a boost if the noise threshold is exceeded (c.f. function `amp_scalefac_bands` in `quantize.c`). In CBR, when increasing quality for some bands, the encoder can not simply select a higher bitrate to accommodate the boosted band(s), so the non-boosted bands will be quantized more heavily or collapse completely.

With `-q 4` (or even faster settings), `noise_shaping_amp` is set to `0`. In this mode, all bands that exceed the noise threshold receive boost. In bitrate-constrained operation (where it's impossible to select a higher bitrate), this seems to spread quantization more evenly across bands and generally avoids that bands collapse completely.

The fix appears to be rather easy: Setting `noise_shaping_amp` to `0` for CBR encodes avoids the observed quality degradation in my tests.

## Discussion

* ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

  [maikmerten](/u/maikmerten/profile/)
  - *2024-06-22*

  For some reason, I accidentally filed this in group "Compatibility". More fitting would be "Quality", of course.

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

  [maikmerten](/u/maikmerten/profile/)
  - *2024-06-22*

  Here's a trivial patch that sets `noise_shaping_amp` to `0` for CBR encoding.

  [cbr-no-noise-shaping-amp.diff](/p/lame/bugs/_discuss/thread/09af39fc6a/19df/attachment/cbr-no-noise-shaping-amp.diff)

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

  [maikmerten](/u/maikmerten/profile/)
  - *2024-07-10*

  ABR seems to be affected similarly to CBR. Here's a slightly expanded trivial patch to disable `noise_shaping_amp` for ABR as well.

  [cbr-and-abr-no-noise-shaping-amp.diff](/p/lame/bugs/_discuss/thread/09af39fc6a/248e/attachment/cbr-and-abr-no-noise-shaping-amp.diff)

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

  [maikmerten](/u/maikmerten/profile/)
  - *2024-07-13*

  The problem of q0/q1/q2/q3 producing worse results for CBR/ABR was introduced with this change:

  <https://sourceforge.net/p/lame/svn/6147/>

  This enabled the new VBR-psymodel for CBR and ABR, which happened to help with <https://sourceforge.net/p/lame/bugs/392/> (which also is about lower speed settings introducing artifacts).

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

  [maikmerten](/u/maikmerten/profile/)
  - *2024-07-25*

  Here's another patch approach: This makes -q4 the lowest quality setting (providing the best quality) for CBR and ABR. This is similar how for the new VBR mode, some quality settings are disallowed as well (a few lines prior).

  [cbr-abr-quality-settings-clamp.diff](/p/lame/bugs/_discuss/thread/09af39fc6a/a26d/attachment/cbr-abr-quality-settings-clamp.diff)

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![Paul Smith](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "Paul Smith")

  [Paul Smith](/u/dishnt1/profile/)
  - *2025-02-07*

  Last edit: Paul Smith 2025-05-28

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

  + ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

    [maikmerten](/u/maikmerten/profile/)
    - *2025-02-09*

    This should not be platform-specific.

    Note, however, that at a high bitrate such as 320 kbps, it's rather unexpected to find cases where one "sounds way better" than the other. Did you check your findings via ABX testing?

    If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

* ![Paul Smith](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "Paul Smith")

  [Paul Smith](/u/dishnt1/profile/)
  - *2025-05-28*

  fantastic patch here i got to say can i ask exactly what modifications you made? cause even at 320kpbs -q0 i can hear a difference with your version i couldn't with my airpods.. but then tested with focal lensys pro studio headphones and you can really notice it then even at 320kbps.

  i mix and master music for a living and your patch version might be the best quality mp3 ive ever heard atleast for CBR anyway

  is it safe to use to this version? cause i want to start using it going forward

  and this patch needs to be implemented in the next version of LAME to be honest its a critical bug

  If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

  + ![maikmerten](https://a.fsdn.com/con/images/sandiego/icons/default-avatar.png "maikmerten")

    [maikmerten](/u/maikmerten/profile/)
    - *2025-05-28*

    The patch disables a quality optimization that targeted the old psychoacoustic model. The old model has been removed years ago, but the optimization has been kept in place and reduces quality with the new model.

    The patch should be very safe to use, but you can also just use the latest official version and disable the offending code by specifying the "-q 4" command line switch.

    If you would like to refer to this comment somewhere else in this project, copy and paste the following link:

---
