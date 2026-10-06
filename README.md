# WXL Character Creation Preview

[Build compatibility and release gate](BUILDING.md)

WXL v1.1 module for class showcases on the WotLK 3.3.5a character-creation screen.

The initial character/race model waits for its outfit and collection geometry; later class
switches use the shorter readiness path. The preview then plays a class-specific showcase with
synchronized stock WotLK effects. Mage casts Pyroblast and Frostbolt toward opposite upper corners,
summons a Water Elemental, channels native WotLK Blizzard, and finishes with Arcane Explosion.
Warrior performs Battle Shout, Thunder Clap, Slam, and Bladestorm. Hunter begins beside an HD
frost wolf, kneels to place an opening Immolation Trap opposite the pet, and fires Fire Shot and
Arcane Shot missiles into the left-side race-button region. The character
finishes in a persistent combat-ready idle. Race and sex changes show a loading overlay, queue the
active class profile for the next Glue update, and reveal the replacement model only after its
equipment is ready. This avoids adding a redundant synchronous equipment rebuild to the selection
click.

Opening or toggling the customization controls cancels the showcase, clears its visual attachments,
and returns the model to the normal stand pose for appearance edits. The module never rebuilds or
randomizes customization data. At 2.5 seconds it refreshes the selected starting outfit, without
detaching any equipment, so armor and weapons finish rendering before the first showcase begins.
Effect cleanup unlinks only the render contexts owned by this module and never clears a shared
attachment slot. The final ready animation follows the showcase weapon family (1H, 2H, staff/polearm,
or bow) rather than using a caster/melee default.

## Integration boundary

- No GlueXML, FrameXML, MPQ, locale, or server files are shipped or replaced.
- `wxl-runtime >= 1.1.0` installs the bootstrap into every Glue script state.
- The native bridge targets client build `3.3.5.12340` and resolves missing animation sequences
  through the stock `AnimationData` fallback chain.
- Spell visuals use fixed paths, attachment IDs, and animation IDs verified against the local
  WotLK 3.3.5a DBCs. The module has no Retail DB2 or FileDataID runtime dependency.
- Visuals are created in the Glue character model's own scene and use stock
  `SpellVisualKit` attachment IDs. Every attachment is tracked and detached before the model is
  rebuilt or the showcase returns to its ready idle.
- Set `wxlCharacterCreatePreview` to `0` to disable the choreography.

## Mage pilot sequence

The Mage profile is the nine-stage reference implementation:

1. Pyroblast precast uses spell 11366 visual 2253, kit 30, and animation 51.
2. Pyroblast release uses kit 38, animation 53, and `PyroBlast_Missile.mdx`. The missile is
   base-anchored and moves toward the upper-left race-button region over 900 ms.
3. Frostbolt precast uses spell 116 visual 13, kit 194, and animation 51.
4. Frostbolt release uses kit 202, animation 53, and `Frostbolt.mdx`, travelling toward the
   top-left. Impact kit 4991 appears at its destination as the next summon begins.
5. Summon Water Elemental precast uses spell 31687 kit 201 and animation 52.
6. The summon cast uses kit 7110, animation 54, `WaterElemental_Impact_Base.mdx`, and the classic
   WotLK Water Elemental model placed to the character's left. The Elemental remains present through
   Blizzard and is cleared when Arcane Explosion begins.
7. Blizzard precast uses spell 10 kit 197 and animation 52.
8. Blizzard channels with kit 717 and animation 125. Its WotLK procedure 9 is reproduced by
   emitting `Blizzard_Impact_Base.mdx` five times per second across the preview for 3.2 seconds.
9. Arcane Explosion uses spell 1449 kit 1004, animation 54, and
   `ArcaneExplosion_Base.mdx`.

Attached effects are moved in the post-engine per-model update event. Each non-zero position is
rebuilt absolutely from the character's root matrix: lateral travel follows the model's Y basis,
depth follows X, and height follows Z. This prevents repeated callbacks from accumulating offsets
and keeps the parent attachment from overwriting missile travel or Blizzard/Water Elemental
placement. CreatureDisplayInfo's type-2
`WaterElementalSkin.blp` is bound explicitly because a bare creature M2 has no unit display
controller to supply its variation texture in the Glue scene.

## Warrior and Hunter test sequences

Warrior uses four native animation/effect stages: BattleRoar (55) with
`BattleShout_Cast_Base.mdx`, Special2H (58) with `ThunderClap_Cast_Base.mdx`, Attack2H (18) with
ground/chest impact effects, and Whirlwind (126) with `Warrior_BladeStorm.m2`.

Hunter uses KneelStart (114), KneelLoop (115), and KneelEnd (116) while placing the classic WotLK
`World\Goober\G_ImmolationTrap.mdx`. The Draenor HD frost wolf remains beside the actor with its
type-11 display texture rebound every frame. The ranged sequence is LoadBow (105), HoldBow (109),
and FireBow (47), giving Aimed Shot a visible held aim without AttackBow's premature release.
Explosive Shot uses a clean LoadBow/FireBow follow-up. Both missile origins are captured from the
actor's left-hand attachment, so their launch height follows each race rather than using a fixed
root-relative height. AttackThrown (107) finishes with spell 1543's WotLK
`Spells\missile_flare.mdx`. The wolf and trap remain fixed until ReadyBow.

Paladin uses a dedicated six-stage WotLK sequence. SpellCastArea (33) places
`Consecration_Impact_Base.mdx` on the ground; Avenging Wrath follows with its impact and sustained
wing state. Attack2H (18) combines Seal of the Crusader and Holy impact effects for the melee
strike. ReadySpellDirected/SpellCastDirected (51/53) provide a readable Holy Light precast and
release. The finisher layers `BlessingOfKings_Base.m2`, `HolyDivineShield_State_Base.mdx`, and
`DivineShield_Low_Chest.mdx` into a sustained golden Kings/bubble effect.

Rogue retains its Backstab and Eviscerate openers, then replaces Shadow Dance with a dedicated
finisher. Vanish uses `Vanish_Cast_Base.mdx` while the actor enters StealthStand (120); once the
cloud clears, the character model frame is hidden with Hide() for the stealth pause. It is shown
again exactly when
`ShadowSteps_FX.m2` begins and Attack1HPierce (85) emerges into Backstab. A
Backstab chest impact is placed just in front of the Rogue to represent an invisible opponent.
SpellCastArea (33) then finishes with the stock WotLK `FanOfKnives_Precast.m2`,
`FanOfKnives.m2`, and `FanOfKnives_Missile.m2` effects.

Priest starts with ChannelCastDirected (124) and launches exactly three Penance missiles at
0.85-second intervals using the same fixed, base-relative top-left trajectory as Mage Pyroblast.
Power Word: Shield follows with the classic Holy Word Shield base/chest
states. Shadowform then activates, the modern `Priest_Psyfiend.m2` spell creature appears beside
the actor, and a 3.2-second Mind Flay approximation combines the authentic WotLK channel/target
effects with three slower stock Shadow Bolt pulses toward that same target while Shadowform and Psyfiend
remain visible. Psyfiend keeps its original TXID-bearing model and resolves all six texture paths
through DB2Gen-generated FileDataID tables; no preview-specific texture-slot overrides are applied.

Shaman hides the actor behind a two-second stock Ghost Wolf form, returns to player form, and places
the four WotLK elemental totems at the corners around the model. Windfury and fire-imbued melee beats
follow. Lightning Bolt and Lava Burst each use a separate SpellPrecastDirected preparation with
matching hand effects before SpellCastDirected releases the missile toward the established left-side
target.

Warlock uses a dedicated five-stage sequence. A stock red-skinned Imp appears beside the
player and performs three slow Firebolt cycles; each cycle visibly prepares for one second before
release and pauses before the next preparation. The player performs a directed casting beat,
charges with the original retail `cfx_warlock_chaosbolt_precasthand.m2`, and releases
`cfx_warlock_chaosbolt_missile.m2` toward the shared top-left target. Rain of Fire replaces Hellfire as the
finisher: the player channels with sustained fire precast/cast effects on both hands while twelve
`RainOfFire_Missile.m2` effects fall around the model over the persistent WotLK ground impact.

Druid uses a shapeshift cloud before each form swap, then bear swipe/roar, cat swipe/Mangle, moonkin Wrath/Moonfire, and tree-form Rejuvenation followed by an eight-second Tranquility channel with area effects. Stock forms retain explicit creature skins and animate independently while the player is hidden; cleanup restores the original player. Native stage validation supports the full Druid sequence. Version 0.17.1 is the accepted visual checkpoint for the next class-by-class review.

## Integration and release checks

This repository carries the `wxl-character-creation-preview` module from the WXL v1.1 integration checkout at `b14c80943b500cd006bd86b3383f67deff238dce`. The repository name is shorter than the extension ID; keep the DLL, manifest ID, and installed folder named `wxl-character-creation-preview`. Build it against the matching Win32 WXL core/API and `wxl-runtime >= 1.1.0`. The manifest deploys the DLL only. The guide above describes the Glue character-creation behavior and the stock versus external visual dependencies.

Before release, build the exact candidate with its pinned core and Runtime, inspect the resulting DLL and Hub manifest, and test each class sequence, race/sex switching, customization cancellation, effect cleanup, and a client restart. A source snapshot is not runtime acceptance. Roll back by closing the client and restoring the prior compatible DLL and Runtime/core pair. Do not bundle client models, textures, MPQs, or server data from a local installation.

## Credits and license

Preserve the WarcraftXL source notices and this repository's GPL-3.0 license. The local v1.1 character-creation integration and class showcase commits are credited to Furioz in Git history. Game models, textures, spell visuals, and other external assets remain their respective owners' work; none are included in this source package.
