// Reads the deck and the client kit through the Smash&Clash SDK, for the WAD baker.
//   node tools/fetch-deck.mjs        -> build/deck.json
import { mkdir, writeFile } from 'node:fs/promises';
import { SmashAndClash } from '../web/sdk/index.js';

const sc = new SmashAndClash({ client: 'smashandclash-doom-build/0.1.0' });
const [cards, kit] = await Promise.all([sc.cards(), sc.assets()]);
await mkdir(new URL('../build/', import.meta.url), { recursive: true });
await writeFile(new URL('../build/deck.json', import.meta.url), JSON.stringify({ cards, kit }, null, 1));
console.log(`deck: ${cards.length} cards, kit v${kit.version}`);
