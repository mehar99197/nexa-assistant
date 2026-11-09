# Third-party notices

Nexa is built on the work of others. Where a part of this repository is taken from, or derived from, someone else's
work under a licence that asks for its notice to travel with it, the notice is here.

## OmniRoute — MIT License, Copyright (c) 2026 diegosouzapw

What is derived from it: the provider catalogue behind Settings → Providers → Add. `scripts/generate-provider-catalog.py`
reads the provider registry and provider metadata of the `omniroute` npm package (version 3.8.51 when the data was
generated; <https://github.com/diegosouzapw/OmniRoute>) at development time and writes `core/ai/ProviderCatalogData.cc`:
provider ids, display names, websites, base URLs, free-tier flags and short notes, as static data. Nothing of OmniRoute
runs in Nexa, and Nexa does not use OmniRoute's gateway, its sign-in methods or its code.

The licence text, as shipped with OmniRoute:

```
MIT License

Copyright (c) 2026 diegosouzapw

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
