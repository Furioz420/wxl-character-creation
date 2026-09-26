# Character Creation Preview

Adds class-specific animation showcases to the WotLK character-creation model. The module injects
its Glue lifecycle through WXL Runtime, so it does not replace character-creation UI or locale files.

The preview covers the ten stock WotLK classes, safely falls back when a race model lacks a requested
animation sequence, and synchronizes two basic effects plus a stronger signature effect per class.
Each sequence waits for and refreshes the starting outfit, performs all three showcases, and
remains combat-ready until the appearance editor is used. Customization immediately returns the model to stand. Visual
attachments are owned by the Glue scene and removed before class, race, or sex model rebuilds.
Cleanup removes only preview-owned effects, preserving collection and outfit attachments that share
the character scene. The final combat-ready pose matches each class showcase weapon family.
