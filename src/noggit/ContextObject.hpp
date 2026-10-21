#ifndef NOGGIT_CONTEXTOBJECT_HPP
#define NOGGIT_CONTEXTOBJECT_HPP

namespace Noggit
{
  enum NoggitRenderContext
  {
    MAP_VIEW,
    ASSET_BROWSER,
    ASSET_BROWSER_PREVIEW,
    OBJECT_PALETTE_PREVIEW,
    PRESET_EDITOR,
    PRESET_EDITOR_PREVIEW,
    GROUND_EFFECT_PREVIEW,
    BLP_RENDERER,
    count
  };

  //! Human readable name of a render context, for diagnostics.
  //!
  //! The asset managers are global and shared by every context, so a report that
  //! only lists file keys cannot say whether an entry is leaked by the map view
  //! or simply still cached by one of the preview renderers.
  inline char const* render_context_name(NoggitRenderContext context)
  {
    switch (context)
    {
    case MAP_VIEW: return "map view";
    case ASSET_BROWSER: return "asset browser";
    case ASSET_BROWSER_PREVIEW: return "asset browser preview";
    case OBJECT_PALETTE_PREVIEW: return "object palette preview";
    case PRESET_EDITOR: return "preset editor";
    case PRESET_EDITOR_PREVIEW: return "preset editor preview";
    case GROUND_EFFECT_PREVIEW: return "ground effect preview";
    case BLP_RENDERER: return "blp renderer";
    default: return "unknown";
    }
  }
}

#endif //NOGGIT_CONTEXTOBJECT_HPP
